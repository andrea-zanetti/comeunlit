#!/usr/bin/env python3
"""
sbctl - signed command-line client for the SimpleBus bridge API v1.

Every request is signed and every response is verified (docs/API.md,
section 3). The key comes from the same secrets.toml the firmware was built
with. Only the Python standard library is used (3.11+ for tomllib).

    sbctl health
    sbctl get 2:6
    sbctl up 2:6                 # also: down, stop
    sbctl batch up 2:3 2:4 2:5
    sbctl scan 2 1-32
    sbctl events                 # wall switches and acks, one JSON per line
    sbctl frames                 # every frame on every bus
    sbctl tx 2 "10 06 01 01"     # raw frame, checksum appended
    sbctl tx 2 --bits 0101...    # raw symbols as given

Bridge address: --host or $SB_HOST (and --port / $SB_PORT, default 80).
Key file: --secrets or $SB_SECRETS, else ./secrets.toml, else the repository
root next to this script.

Exit status: 0 success, 1 the bridge reported an error, 2 usage or
connection problem, 3 authentication / signature problem.
"""

import argparse
import hashlib
import hmac
import http.client
import json
import os
import sys
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover
    import tomli as tomllib

ZERO_SESSION = "0" * 16


class AuthError(Exception):
    """Signature missing or wrong, or the session could not be established."""


class StreamStalled(Exception):
    """No signed heartbeat within STALL_S: stream stalled or withheld."""


STALL_S = 31  # API.md 3.6: no hb for 30 s = stalled


# ─── Signing (must match Core/Src/sb_auth.c) ──────────────────────────────────

def sign_request(key: bytes, method: str, path: str, session: str,
                 counter: int, body: bytes = b"") -> str:
    msg = f"SB1-REQ\n{method}\n{path}\n{session}\n{counter}\n".encode() + body
    return hmac.new(key, msg, hashlib.sha256).hexdigest()


def sign_response(key: bytes, request_mac: str, status: int,
                  body: bytes) -> str:
    msg = f"SB1-RSP\n{request_mac}\n{status}\n".encode() + body
    return hmac.new(key, msg, hashlib.sha256).hexdigest()


def sign_event(key: bytes, request_mac: str, seq: int, event: str,
               data: bytes) -> str:
    msg = f"SB1-EVT\n{request_mac}\n{seq}\n{event}\n".encode() + data
    return hmac.new(key, msg, hashlib.sha256).hexdigest()


# ─── Client ───────────────────────────────────────────────────────────────────

class Bridge:
    def __init__(self, host: str, port: int, key: bytes, timeout: float = 10):
        self.host, self.port, self.key, self.timeout = host, port, key, timeout
        self.session = ZERO_SESSION
        self.counter = 1

    def _send(self, method, path, body):
        counter = self.counter
        self.counter += 1
        req_mac = sign_request(self.key, method, path, self.session, counter,
                               body or b"")
        headers = {
            "X-SB-Session": self.session,
            "X-SB-Counter": str(counter),
            "X-SB-MAC": req_mac,
        }
        if body is not None:
            headers["Content-Type"] = "application/json"
        conn = http.client.HTTPConnection(self.host, self.port,
                                          timeout=self.timeout)
        try:
            conn.request(method, path, body=body, headers=headers)
            resp = conn.getresponse()
            data = resp.read()
            return resp.status, resp.getheader("X-SB-MAC"), data, req_mac
        finally:
            conn.close()

    def request(self, method: str, path: str, obj=None):
        """Signed request; returns (status, parsed JSON). Verifies the answer
        and transparently adopts a new session from a signed 401."""
        body = None if obj is None else json.dumps(
            obj, separators=(",", ":")).encode()

        for _ in range(3):
            status, mac, data, req_mac = self._send(method, path, body)
            signed = (mac is not None and hmac.compare_digest(
                mac, sign_response(self.key, req_mac, status, data)))
            if mac is not None and not signed:
                raise AuthError(f"response signature INVALID (HTTP {status})")

            if status == 401:
                if not signed:
                    raise AuthError(
                        "unsigned 401: the bridge rejected our signature "
                        "(wrong key?) " + data.decode(errors="replace"))
                info = json.loads(data)
                self.session = info["session"]
                self.counter = int(info["next"])
                continue

            if not signed:
                raise AuthError(f"unsigned response, HTTP {status}: "
                                + data.decode(errors="replace"))
            return status, json.loads(data) if data else None

        raise AuthError("could not establish a session")

    def stream(self, path: str):
        """Open a signed stream and yield (event, obj) for every verified
        event. Raises AuthError on any signature or sequence problem,
        StreamStalled if the heartbeat stops."""
        for _ in range(3):
            counter = self.counter
            self.counter += 1
            req_mac = sign_request(self.key, "GET", path, self.session,
                                   counter)
            conn = http.client.HTTPConnection(self.host, self.port,
                                              timeout=STALL_S)
            conn.request("GET", path, headers={
                "X-SB-Session": self.session,
                "X-SB-Counter": str(counter),
                "X-SB-MAC": req_mac,
            })
            resp = conn.getresponse()
            mac = resp.getheader("X-SB-MAC")

            if resp.status != 200:
                data = resp.read()
                conn.close()
                signed = (mac is not None and hmac.compare_digest(
                    mac, sign_response(self.key, req_mac, resp.status, data)))
                if mac is not None and not signed:
                    raise AuthError(
                        f"response signature INVALID (HTTP {resp.status})")
                if resp.status == 401 and signed:
                    info = json.loads(data)
                    self.session = info["session"]
                    self.counter = int(info["next"])
                    continue
                if not signed:
                    raise AuthError(f"unsigned response, HTTP {resp.status}: "
                                    + data.decode(errors="replace"))
                raise AuthError(f"stream refused, HTTP {resp.status}: "
                                + data.decode(errors="replace"))

            if mac is None or not hmac.compare_digest(
                    mac, sign_response(self.key, req_mac, 200, b"")):
                conn.close()
                raise AuthError("stream header signature INVALID")

            try:
                yield from self._events(resp, req_mac)
            finally:
                conn.close()
            return
        raise AuthError("could not establish a session")

    def _events(self, resp, req_mac):
        expected = 1
        event = seq = data = None
        while True:
            try:
                raw = resp.fp.readline()
            except TimeoutError:
                raise StreamStalled(f"no data for {STALL_S} s")
            if not raw:
                return  # bridge closed the stream
            line = raw.rstrip(b"\r\n")
            if line.startswith(b"event: "):
                event = line[7:].decode()
            elif line.startswith(b"id: "):
                seq = int(line[4:])
            elif line.startswith(b"data: "):
                data = line[6:]
            elif line == b"" and event is not None:
                if seq != expected:
                    raise AuthError(f"event sequence broken: got id {seq}, "
                                    f"expected {expected}")
                mac, _, payload = data.partition(b" ")
                if not hmac.compare_digest(
                        mac.decode(), sign_event(self.key, req_mac, seq,
                                                 event, payload)):
                    raise AuthError(f"event {seq} signature INVALID")
                expected += 1
                yield event, json.loads(payload)
                event = seq = data = None


# ─── CLI ──────────────────────────────────────────────────────────────────────

def load_key(path_arg):
    candidates = []
    if path_arg:
        candidates = [Path(path_arg)]
    elif os.environ.get("SB_SECRETS"):
        candidates = [Path(os.environ["SB_SECRETS"])]
    else:
        candidates = [Path("secrets.toml"),
                      Path(__file__).resolve().parent.parent / "secrets.toml"]
    for p in candidates:
        if p.is_file():
            with open(p, "rb") as f:
                key = tomllib.load(f).get("auth", {}).get("key", "")
            try:
                raw = bytes.fromhex(key)
            except ValueError:
                raw = b""
            if len(raw) != 32:
                sys.exit(f"sbctl: {p}: [auth] key must be 64 hex digits")
            return raw
    sys.exit("sbctl: no secrets.toml found (use --secrets or $SB_SECRETS)")


def parse_target(t):
    try:
        b, a = t.split(":")
        return {"bus": int(b), "addr": int(a)}
    except ValueError:
        sys.exit(f"sbctl: target must be BUS:ADDR, got {t!r}")


def parse_range(r):
    try:
        lo, hi = (int(x) for x in r.split("-")) if "-" in r else (int(r),) * 2
    except ValueError:
        sys.exit(f"sbctl: range must be FIRST-LAST, got {r!r}")
    if not (0 <= lo <= hi <= 255):
        sys.exit("sbctl: addresses are 0-255")
    return lo, hi


def show(status, obj):
    print(json.dumps(obj, indent=2))
    if status != 200:
        return 1
    if isinstance(obj, dict) and any("error" in r for r in obj.get("results", [])):
        return 1
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="sbctl", description="Signed client for the SimpleBus bridge.")
    ap.add_argument("--host", default=os.environ.get("SB_HOST"))
    ap.add_argument("--port", type=int,
                    default=int(os.environ.get("SB_PORT", "80")))
    ap.add_argument("--secrets", help="path to secrets.toml")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("health")
    p = sub.add_parser("get"); p.add_argument("target")
    for c in ("up", "down", "stop"):
        p = sub.add_parser(c); p.add_argument("target")
    p = sub.add_parser("batch")
    p.add_argument("action", choices=["up", "down", "stop"])
    p.add_argument("targets", nargs="+")
    p = sub.add_parser("scan")
    p.add_argument("bus", type=int)
    p.add_argument("range", help="FIRST-LAST, e.g. 1-32")
    for c in ("events", "frames"):
        p = sub.add_parser(c)
        p.add_argument("--count", type=int, default=0,
                       help="stop after N events (heartbeats not counted)")
        p.add_argument("--hb", action="store_true",
                       help="also print heartbeats")
    p = sub.add_parser("tx")
    p.add_argument("bus", type=int)
    p.add_argument("hex", nargs="?", help='bytes, e.g. "10 06 01 01"')
    p.add_argument("--bits", help="raw symbols (0/1) instead of hex")
    p.add_argument("--no-checksum", action="store_true",
                   help="do not append the checksum byte to hex")
    args = ap.parse_args(argv)

    if not args.host:
        ap.error("bridge address needed: --host or $SB_HOST")
    br = Bridge(args.host, args.port, load_key(args.secrets))

    try:
        if args.cmd == "health":
            return show(*br.request("GET", "/api/v1/health"))
        if args.cmd == "get":
            t = parse_target(args.target)
            return show(*br.request("GET",
                                    f"/api/v1/blinds/{t['bus']}/{t['addr']}"))
        if args.cmd in ("up", "down", "stop"):
            t = parse_target(args.target)
            return show(*br.request("POST",
                                    f"/api/v1/blinds/{t['bus']}/{t['addr']}",
                                    {"cmd": args.cmd}))
        if args.cmd == "batch":
            return show(*br.request("POST", "/api/v1/batch", {
                "cmd": args.action,
                "targets": [parse_target(t) for t in args.targets]}))
        if args.cmd == "scan":
            lo, hi = parse_range(args.range)
            found = []
            for addr in range(lo, hi + 1):
                st, obj = br.request("GET", f"/api/v1/blinds/{args.bus}/{addr}")
                if st == 200:
                    found.append(obj)
                    print(f"{args.bus}:{addr}  {obj['state']}", flush=True)
                elif st not in (504,):
                    print(f"{args.bus}:{addr}  HTTP {st} {obj}", flush=True)
            print(f"{len(found)} module(s) answered on bus {args.bus}, "
                  f"addresses {lo}-{hi}")
            return 0
        if args.cmd in ("events", "frames"):
            path = ("/api/v1/events" if args.cmd == "events"
                    else "/api/v1/admin/frames")
            n = 0
            try:
                for event, obj in br.stream(path):
                    if event == "hb":
                        if args.hb:
                            print(json.dumps({"hb": obj}), flush=True)
                        continue
                    print(json.dumps(obj, separators=(",", ":")), flush=True)
                    n += 1
                    if args.count and n >= args.count:
                        return 0
            except KeyboardInterrupt:
                return 0
            except StreamStalled as e:
                print(f"sbctl: stream stalled: {e}", file=sys.stderr)
                return 2
            print("sbctl: stream closed by the bridge", file=sys.stderr)
            return 2
        if args.cmd == "tx":
            if (args.hex is None) == (args.bits is None):
                ap.error("tx needs exactly one of HEX or --bits")
            body = ({"bits": args.bits} if args.bits else
                    {"hex": args.hex, "checksum": not args.no_checksum})
            return show(*br.request("POST", f"/api/v1/admin/tx/{args.bus}",
                                    body))
    except AuthError as e:
        print(f"sbctl: AUTH: {e}", file=sys.stderr)
        return 3
    except (OSError, http.client.HTTPException) as e:
        print(f"sbctl: connection to {args.host}:{args.port} failed: {e}",
              file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    sys.exit(main())
