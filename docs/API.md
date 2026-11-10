# SimpleBus bridge — HTTP API v1

Status: **implemented** (firmware 0.1.0). Everything below is served and
signed; the bridge serves nothing outside `/api/v1/` (plain 404).

## 1. Overview

The bridge is a **stateless actuator**. It owns the SimpleBus lines and the
timing rules of the blind modules; everything else (which blinds exist,
their names, groups, last known state) lives in the home automation system.

* The bridge stores nothing at run time: no device list, no state cache, no
  configuration. The only persistent data is the shared key, compiled in.
* Every state read is a live poll on the bus. Every command reads the state
  first, then decides what to press.
* Frames the bridge observes but did not cause (wall switch presses) are
  decoded and forwarded as events, without being remembered.

Two endpoint groups:

| Group   | Purpose                                         | Endpoints |
|---------|-------------------------------------------------|-----------|
| Control | operate blinds                                  | `GET/POST /blinds/{bus}/{addr}`, `POST /batch`, `GET /events` |
| Admin   | debugging and protocol work                     | `GET /admin/frames`, `POST /admin/tx/{bus}` |
| —       | liveness, bus list, session discovery           | `GET /health` |

All paths are under `/api/v1`. **Every request and every response is
signed** (section 3). There are no HTML pages and no unsigned endpoints.

Transport is plain HTTP/1.1 on port 80. Integrity and authenticity come from
the signatures; confidentiality is explicitly not a goal.

## 2. Conventions

* Bodies are JSON, `Content-Type: application/json`, UTF-8.
* Requests with a body must send `Content-Length`. Maximum body: 512 bytes
  (413 above that).
* `bus` is the **physical bus number** as printed on the shield (2, 3, 4 on
  rev 1). `addr` is the module address, decimal 0–255.
* Blind state is one of `"idle"`, `"up"`, `"down"`, or `"unknown"` when the
  module reports an output value other than 0, 1, 2 (the raw value is then
  included as `out`).
* Errors always have the shape `{"error": "<code>", ...details}`.

| Status | `error`        | Meaning |
|--------|----------------|---------|
| 400    | `bad_request`  | malformed JSON, missing field, bad value |
| 401    | `auth`         | signature, session or counter rejected (section 3.4) |
| 404    | `no_bus`       | bus number not fitted on this shield |
| 404    | `not_found`    | unknown path |
| 405    | `method`       | wrong method for this path |
| 409    | `state`        | module reports a relay value the bridge does not know; nothing pressed |
| 413    | `too_large`    | body over 512 bytes |
| 500    | `tx_failed`    | the bridge's transmitter (timer/DMA) did not complete; nothing was sent, or the frame was cut short |
| 502    | `mismatch`     | the module's ack reports a different state than the press should have produced |
| 503    | `bus_busy`     | listen-before-talk gave up, nothing was sent |
| 503    | `busy`         | too many concurrent streams |
| 504    | `no_reply`     | module did not answer a poll |
| 504    | `no_ack`       | module did not acknowledge a press (details say how far it got) |
| 504    | `timeout`      | the request did not complete within 5 s |

## 3. Authentication

### 3.1 Key

A 32-byte key shared between the bridge and the client. On the bridge side it
comes from `secrets.toml` at build time (section 8); changing it means
reflashing.

### 3.2 Session and counter

* **Session**: 64-bit random value drawn from the hardware RNG at boot,
  written as 16 lowercase hex digits. It changes on every reboot.
* **Counter**: unsigned 64-bit integer chosen by the client, decimal
  (1 or more digits, no sign). Counter 0 is never accepted, so the first
  valid counter in a session is 1.
  For each session the bridge keeps the highest accepted counter and a
  64-entry sliding window below it (same scheme as RFC 4303 §3.4.3):
  * counter above the highest: accepted, window slides;
  * counter inside the window and not yet seen: accepted (allows a few
    requests to arrive out of order);
  * already seen, or below the window: rejected.

The window state lives in RAM only. After a reboot the session changes, so old
requests can never be replayed.

### 3.3 Request signature

Headers on every request:

```
X-SB-Session: 3f9a0c51d2e87b46
X-SB-Counter: 1042
X-SB-MAC:     <64 lowercase hex digits>
```

```
MAC = HMAC-SHA256(key,
        "SB1-REQ\n" + METHOD + "\n" + PATH + "\n" +
        SESSION + "\n" + COUNTER + "\n" + BODY)
```

* `METHOD` uppercase, `PATH` exactly as sent on the request line (including
  any query string), `BODY` the raw body bytes (empty for GET).
* Comparison on the bridge is constant-time.

### 3.4 Bootstrap and resync

A client that does not know the session sends its first request with
`X-SB-Session: 0000000000000000` and any counter. The bridge answers:

```
HTTP/1.1 401
{"error":"auth","reason":"session","session":"3f9a0c51d2e87b46","next":1}
```

`reason` is one of `session` (wrong or unknown session), `replay` (counter
already used or below the window), `mac` (signature invalid, or the three
headers missing or malformed: session not 16 lowercase hex digits, counter
not decimal, MAC not 64 lowercase hex digits).

If the boot self-test of the HASH unit or the RNG failed, every request gets
an unsigned `503 {"error":"crypto"}` and nothing is executed; the console
says which test failed.

`next` is one above the highest accepted counter in the current session. The
client adopts `session` and `next` and retries. The same answer covers a
bridge reboot (new session) and a client restart (counter reset).

The 401 is **signed like any response** (3.5) when the request's MAC was
valid for the session and counter it claimed, so a client holding the key
can tell a genuine resync from a forged one. When `reason` is `mac` the 401
is unsigned: either the key is wrong (configuration error) or the request was
tampered with. Clients must only adopt `session`/`next` from a signed 401.

**One counter sequence per session, shared by all clients.** Two clients
sending requests at the same time race for the same counters: each `next`
can be used by whoever gets there first, and the other gets a replay 401.
A busy client can starve an occasional one (seen on the bench: a request
loop of `sbctl scan` made a second `sbctl` give up after three resyncs).
Keep one client per bridge (normally the home automation server). A stream
needs a counter only to open; once open it is unaffected, so open streams
before starting a request-heavy client.

### 3.5 Response signature

Every response carries `X-SB-MAC`:

```
MAC = HMAC-SHA256(key,
        "SB1-RSP\n" + REQUEST_MAC + "\n" + STATUS + "\n" + BODY)
```

`REQUEST_MAC` is the hex MAC of the request being answered, so a response
cannot be replayed or moved to another request. `STATUS` is the decimal
status code.

### 3.6 Stream signature

Streams (`/events`, `/admin/frames`) are opened with a signed GET and answered
with `200` and `Content-Type: text/event-stream`. The `200` carries
`X-SB-MAC` computed as in 3.5 with an empty body, so the client knows the
stream itself comes from the bridge. Each event is one SSE `data:` line
containing the MAC, a space, and the JSON payload:

```
event: state
id: 17
data: 9b1e…c4 {"bus":2,"addr":6,"state":"up","button":"up","source":"local"}
```

```
MAC = HMAC-SHA256(key,
        "SB1-EVT\n" + REQUEST_MAC + "\n" + SEQ + "\n" + EVENT + "\n" + JSON)
```

* `REQUEST_MAC` is the MAC of the GET that opened the stream, so events
  cannot be spliced between streams.
* `SEQ` (the `id:` field) starts at 1 and increases by exactly 1. A gap or a
  repeat means events were lost or injected: the client must drop the stream
  and reconnect.
* A signed `hb` event is sent when the stream opens and then every 15 s,
  whatever the traffic: `{"uptime_ms":123456}`. No `hb` for 30 s means the
  stream is stalled or being withheld.
* A stream starts at the moment it is opened: frames from before are not
  replayed.

## 4. Common endpoint

### `GET /api/v1/health`

```json
{
  "fw": "0.1.0",
  "shield": "1",
  "buses": [{"bus":2,"term":"J6"},{"bus":3,"term":"J7"},{"bus":4,"term":"J8"}],
  "uptime_ms": 81234567,
  "heap_free": 74210,
  "streams": 1
}
```

## 5. Control API

### 5.1 `GET /api/v1/blinds/{bus}/{addr}` — read state

One live poll on the bus, single attempt.

```json
200 {"bus":2,"addr":6,"state":"up"}
504 {"error":"no_reply","bus":2,"addr":6}
```

Discovery is done by the client: loop this over the address range of each
bus and keep the addresses that answer.

### 5.2 `POST /api/v1/blinds/{bus}/{addr}` — command

```json
{"cmd": "up" | "down" | "stop"}
```

The bridge polls, then applies:

| Current   | `up`            | `down`          | `stop`          |
|-----------|-----------------|-----------------|-----------------|
| `idle`    | press UP        | press DOWN      | nothing         |
| `up`      | nothing         | **reverse**     | press DOWN      |
| `down`    | **reverse**     | nothing         | press UP        |
| `unknown` | refused (`409 {"error":"state","out":N}`) |||

* **press** sends one command frame and waits for the module's
  acknowledgement (frame type `0x00`, same address, `in` = the pressed
  button). The acknowledgement's `out` byte is the new state.
* **reverse** = press the opposite button (stops the motor), wait 500 ms,
  press the target button.
* A STOP is a press in the direction opposite to the current motion.

**No automatic retries.** Every poll and every press is attempted once. A
press is not idempotent (a repeated STOP starts the blind the other way), so
when something is missing the bridge stops and reports how far it got; the
client decides whether to read the state and try again.

Response:

```json
200 {"bus":2,"addr":6,"before":"up","action":"reverse","after":"down"}
200 {"bus":2,"addr":6,"before":"up","action":"none","after":"up"}
504 {"error":"no_reply","bus":2,"addr":6}
504 {"error":"no_ack","bus":2,"addr":6,"before":"up","action":"reverse","done":"none"}
504 {"error":"no_ack","bus":2,"addr":6,"before":"up","action":"reverse","done":"stop","after":"idle"}
```

* `action` is `none`, `press` or `reverse`: what the bridge decided to do.
* On `no_ack`, `done` is the last step that was acknowledged (`none` or
  `stop`) and `after` is the state from that acknowledgement, when there is
  one. A missing acknowledgement does not mean the press had no effect: the
  module may have acted and the acknowledgement been lost, so the state is
  unknown until the client reads it.

### 5.3 `POST /api/v1/batch` — synchronized group command

```json
{"cmd":"up","targets":[{"bus":2,"addr":3},{"bus":2,"addr":4},{"bus":2,"addr":5}]}
```

At most 16 targets, no duplicates. Execution:

1. poll every target;
2. send every stop press (targets that need a stop or a reversal);
3. if any reversal is pending, one shared 500 ms pause;
4. send every direction press.

Each phase goes through the targets in the order given. The response always
has one result per target, in the same order, using the 5.2 shapes:

```json
200 {"results":[
  {"bus":2,"addr":3,"before":"idle","action":"press","after":"up"},
  {"bus":2,"addr":4,"before":"down","action":"reverse","after":"up"},
  {"bus":2,"addr":5,"error":"no_reply"}
]}
```

The status is 200 even when individual targets fail; the per-target entries
carry the errors.

### 5.4 `GET /api/v1/events` — observed state changes (stream)

Forwards acknowledgement/state frames (type `0x00`) seen on any bus, decoded:

```json
{"bus":2,"addr":6,"state":"up","button":"up","source":"local"}
```

* `button`: `up`, `down`, or `none`, from the frame's `in` byte.
* `state`: from the frame's `out` byte, as in section 2.
* `source`: `bridge` if the frame acknowledges a press the bridge sent in the
  previous 200 ms, otherwise `local` (wall switch or another master).
* Modules repeat these frames (the wall-switch capture shows each one twice,
  131 ms and 252 ms apart). The bridge drops a frame identical to the last
  one from the same bus and address within 500 ms.

The bridge does not remember anything beyond that 500 ms comparison.

Modules do **not** announce the end of travel: when the relay switches off on
its own timer, nothing appears on the bus. A client that needs to know a
blind has stopped must poll it with 5.1. Real-time position or travel
tracking is out of scope for v1.

## 6. Admin API

### 6.1 `GET /api/v1/admin/frames` — raw frame stream

Event name `frame`, one per frame on any bus:

```json
{"ts":2904247,"bus":2,"dir":"rx","bits":"0101…","proto":"STD","hex":"00 06 01 01 5D","chk_ok":true}
```

* `dir`: `rx` received, `tx` sent by the bridge, `drop` not sent (bus busy).
* `ts`: bridge uptime in ms when the frame was published.
* `proto`, `hex` and `chk_ok` are present when the bits start with a known
  preamble (`STD` seed `0x55`, `EXT` `0x77`, `RSP` `0x33`) and at least 5
  bytes decode; otherwise only `bits` is sent.

### 6.2 `POST /api/v1/admin/tx/{bus}` — raw transmit

```json
{"hex":"10 06 01 01","checksum":true}
{"bits":"01010101010000…"}
```

Exactly one of:

* `hex`: 1 to 8 bytes, spaces optional, sent with the STD preamble;
  `"checksum":true` appends the seed-`0x55` checksum (default `false`);
* `bits`: 1 to 128 symbols of `0`/`1`, sent exactly as given.

The call takes the bus like any other user, sends, keeps the bus for
150 ms so a module's reply is not talked over, and returns:

```json
200 {"sent":true,"bits":59}
503 {"error":"bus_busy"}
```

## 7. Bus access

* Each bus has a lock. A command holds it from its poll to its last press
  (including the reversal pause), a batch holds every bus it touches for the
  whole batch, and raw transmissions take it too. Frames from different
  callers never interleave. A caller that cannot get a bus within 5 s gets
  `timeout`.
* Listen-before-talk is always on: before each frame the line must be idle for
  2 ms, giving up after 50 ms (`bus_busy`).
* A poll or press waits up to 150 ms for the reply. Measured: about 28 ms
  from the end of the bridge's frame to the complete reply.
* The bridge leaves at least 40 ms of quiet after each reply (or timeout)
  before its next frame on the same bus.
* An ack whose `out` differs from what the press should produce is reported
  as `mismatch` (502) with the reported state in `after`.
* At most 4 streams are open at once, across `/events` and `/admin/frames`;
  a fifth gets a signed `503 {"error":"busy"}`.

## 8. Build-time secret

`secrets.toml` at the project root, **never committed** (listed in
`.gitignore`; `secrets.example.toml` is committed):

```toml
[auth]
key = "<64 hex digits>"
```

Generate a key with:

```sh
python3 -c "import secrets; print(secrets.token_hex(32))"
```

`cmake/gen_secrets.py` turns it into `build/generated/sb_secret.h`. The build
fails with a message pointing to this section when the file is missing or the
key is not exactly 32 bytes.

## 9. Reference client

`tools/sbctl.py` (Python 3.11+, standard library only) signs requests and
verifies every response; it refuses anything unsigned or wrongly signed.

```sh
export SB_HOST=<bridge address>       # or --host
export SB_SECRETS=/path/secrets.toml  # or --secrets; default ./secrets.toml
sbctl health
sbctl get 2:6
sbctl up 2:6            # also: down, stop
sbctl batch up 2:3 2:4 2:5
sbctl scan 2 1-32
sbctl events [--count N] [--hb]    # one JSON object per line
sbctl frames [--count N] [--hb]
sbctl tx 2 "10 06 01 01"           # checksum appended (--no-checksum)
sbctl tx 2 --bits 0101...
```

Streams are verified event by event: signature, and `id` exactly one more
than the previous. Any failure ends the command with exit status 3; no
heartbeat for 31 s ends it with 2.

Exit status: 0 success, 1 the bridge reported an error, 2 usage or
connection problem (or a stream closed / stalled), 3 authentication or
signature problem.

Signing a request in Python:

```python
import hmac, hashlib

def sign_request(key, method, path, session, counter, body=b""):
    msg = (f"SB1-REQ\n{method}\n{path}\n{session}\n{counter}\n").encode() + body
    return hmac.new(key, msg, hashlib.sha256).hexdigest()

def verify_response(key, request_mac, status, body, mac):
    msg = f"SB1-RSP\n{request_mac}\n{status}\n".encode() + body
    return hmac.compare_digest(hmac.new(key, msg, hashlib.sha256).hexdigest(), mac)
```

## 10. Wire protocol reference

The bus protocol, with the scope figure, the verified button-press
semantics and timing, is documented in
[simplebus_protocol_reference.md](simplebus_protocol_reference.md). The part
the API relies on:

| Type   | Direction        | Meaning |
|--------|------------------|---------|
| `0x21` | master → module  | poll |
| `0x01` | module → master  | poll reply; `out` = relay state (0 idle, 1 up, 2 down) |
| `0x10` | master → module  | button press; `01 01` UP, `02 02` DOWN (`00 00` is a no-op) |
| `0x00` | module → all     | announcement: `in` = button pressed, `out` = new state. Once after a bridge press (the ack), twice after a wall-switch press. Not sent at end of travel |

## 11. Open items

* The 150 ms reply timeout, the 40 ms inter-frame quiet time and the 500 ms
  reversal pause are initial values to tune against captures.
* Modules repeat wall-switch announcements but, in the 2026-11-08 capture
  (single commands, a reversal and a 3-blind batch), sent exactly one ack per
  bridge press. Recheck if batches ever show garbled frames: a repeat
  starting while the bridge sends to the next module would collide
  (listen-before-talk only checks before a frame starts).

Out of scope for v1: automatic retries, real-time travel/position tracking.
