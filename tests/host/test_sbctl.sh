#!/usr/bin/env bash
# End-to-end: tools/sbctl.py against fake_bridge (firmware API code on a
# host socket). Run via: make -C tests/host e2e
set -u
cd "$(dirname "$0")"
PORT=${PORT:-18080}
SBCTL="python3 ../../tools/sbctl.py --host 127.0.0.1 --port $PORT"
TMP=$(mktemp -d)
trap 'kill $BRIDGE 2>/dev/null; rm -rf "$TMP"' EXIT

# Same key as crypto_host.c (bytes 0x00..0x1f)
printf '[auth]\nkey = "%s"\n' \
  000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f > "$TMP/good.toml"
printf '[auth]\nkey = "%s"\n' \
  ff0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f > "$TMP/bad.toml"

./fake_bridge "$PORT" > "$TMP/bridge.log" 2>&1 &
BRIDGE=$!
sleep 0.5

fail=0; n=0
expect() {  # expect <exit-code> <grep-pattern|-> <args...>
  local want=$1 pat=$2; shift 2
  n=$((n+1))
  out=$($SBCTL "$@" 2>&1); rc=$?
  if [ "$rc" != "$want" ] || { [ "$pat" != "-" ] && ! grep -q -- "$pat" <<<"$out"; }; then
    echo "FAIL: sbctl $* -> exit $rc (want $want), pattern '$pat'"; echo "$out" | sed 's/^/    /'
    fail=$((fail+1))
  fi
}

export SB_SECRETS="$TMP/good.toml"
expect 0 '"fw": "0.1.0"'            health
expect 0 '"state": "idle"'          get 2:6
expect 0 '"action": "press"'        up 2:6
expect 0 '"action": "none"'         up 2:6
expect 0 '"action": "reverse"'      down 2:6
expect 0 '"before": "down"'         stop 2:6
expect 1 '"error": "no_reply"'      get 2:9
expect 1 '"error": "no_bus"'        get 1:6
expect 0 '"action": "reverse"'      batch up 2:3 2:4 2:5
expect 1 '"error": "no_reply"'      batch up 2:3 2:9
expect 0 '4 module(s) answered'     scan 2 1-10

# Streams: hb (seq 1) + bridge ack + one wall-switch event (repeat dropped)
out=$($SBCTL events 2>/dev/null)
n=$((n+1))
want='{"bus":2,"addr":6,"state":"up","button":"up","source":"bridge"}
{"bus":2,"addr":5,"state":"idle","button":"down","source":"local"}'
if [ "$out" != "$want" ]; then echo "FAIL: events"; echo "$out" | sed 's/^/    /'; fail=$((fail+1)); fi

expect 0 '"source":"bridge"'        events --count 1
expect 0 '"hex":"21 06 00 00 7C"'   frames --count 6
expect 0 '"hb"'                     frames --hb --count 1
expect 0 '"bits": 59'               tx 2 "10 06 01 01"
expect 0 '"bits": 49'               tx 2 "10 06 01 01" --no-checksum
expect 0 '"bits": 4'                tx 3 --bits 0101
expect 1 '"error": "no_bus"'        tx 1 "10"
expect 1 '"error": "bad_request"'   tx 2 "1g"

# A tampered event must be rejected
kill $BRIDGE; wait $BRIDGE 2>/dev/null
FAKE_CORRUPT=1 ./fake_bridge "$PORT" > "$TMP/bridge2.log" 2>&1 &
BRIDGE=$!
sleep 0.5
expect 3 'event 3 signature INVALID' events

export SB_SECRETS="$TMP/bad.toml"
expect 3 'wrong key'                health
unset SB_SECRETS

expect 3 'wrong key'                --secrets "$TMP/bad.toml" up 2:6

echo "sbctl e2e: $n checks, $fail failures"
[ $fail -eq 0 ]
