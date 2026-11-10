#!/usr/bin/env python3
"""
gen_pins.py - generate the SimpleBus shield pin header.

Resolution chain, per bus and per signal:

    bus signal -> shield pin (J4.5) -> Nucleo pin (CN10.10) -> GPIO (PE13)

Inputs (TOML):
    --board   board map:  Nucleo connector pin -> GPIO (+ capture functions)
    --mating  mating map: shield connector pin -> Nucleo connector pin
    --shield  shield map: physical bus id -> shield pins for rx / data / oe

Output:
    --out     C header with the resolved per-bus pin table

Bus ids are the PHYSICAL bus numbers from the silkscreen (Bus n = terminal
J(n+4)). They need not be contiguous: a shield may leave a bus out. The
firmware keeps a 0-based table internally; everything outside the firmware
(HTTP API, SSE stream, web pages) uses the physical id.

The build fails (exit 1) if any check fails:
    - a referenced pin is missing from the mating or board map
    - a GPIO name is malformed
    - two signals resolve to the same GPIO
    - bus ids are missing, duplicated or outside 1..99
    - an RX pin has no timer capture function
    - RX pins are spread over more than one timer, or share a channel
"""

import argparse
import re
import sys

try:
    import tomllib  # Python 3.11+
except ModuleNotFoundError:  # pragma: no cover
    import tomli as tomllib  # pip install tomli

SIGNALS = ("rx", "data", "oe")
GPIO_RE = re.compile(r"^P([A-K])(\d{1,2})$")
TERM_RE = re.compile(r"^[A-Za-z0-9_.-]{1,8}$")


class PinMapError(Exception):
    pass


def load(path):
    with open(path, "rb") as f:
        return tomllib.load(f)


def parse_gpio(name):
    m = GPIO_RE.match(name)
    if not m or int(m.group(2)) > 15:
        raise PinMapError(f"malformed GPIO name '{name}'")
    return m.group(1), int(m.group(2))


def resolve(board, mating, shield):
    errors = []
    bpins = board.get("pins", {})
    mmap = mating.get("mating", {})
    raw_buses = shield.get("bus", [])

    if not raw_buses:
        raise PinMapError("shield map defines no buses")

    seen_ids = set()
    for b in raw_buses:
        bid = b.get("id")
        if not isinstance(bid, int) or not 1 <= bid <= 99:
            errors.append(f"bus id must be an integer 1..99, got {bid!r}")
        elif bid in seen_ids:
            errors.append(f"bus id {bid} defined twice")
        seen_ids.add(bid)
        term = b.get("term", "")
        if term and not TERM_RE.match(term):
            errors.append(f"Bus {bid}: bad terminal label {term!r}")
    if errors:
        raise PinMapError("\n".join(errors))

    buses = sorted(raw_buses, key=lambda b: b["id"])

    resolved = []
    used = {}  # gpio -> "Bus n <signal>"

    for b in buses:
        entry = {"id": b["id"], "term": b.get("term", "")}
        for sig in SIGNALS:
            where = f"Bus {b['id']} {sig}"
            spin = b.get(sig)
            if spin is None:
                errors.append(f"{where}: missing")
                continue
            npin = mmap.get(spin)
            if npin is None:
                errors.append(f"{where}: shield pin {spin} not in mating map")
                continue
            info = bpins.get(npin)
            if info is None:
                errors.append(f"{where}: {spin} -> {npin} not in board map")
                continue
            gpio = info.get("gpio", "")
            try:
                port, num = parse_gpio(gpio)
            except PinMapError as e:
                errors.append(f"{where}: {e}")
                continue
            if gpio in used:
                errors.append(f"{where}: {gpio} already used by {used[gpio]}")
            used[gpio] = where
            entry[sig] = {
                "shield": spin,
                "nucleo": npin,
                "gpio": gpio,
                "port": port,
                "num": num,
                "label": info.get("label", ""),
                "capture": info.get("capture", []),
            }
        resolved.append(entry)

    # RX: every receiver needs a capture channel, all on one timer,
    # each on a distinct channel.
    rx_timer = None
    rx_channels = {}
    for e in resolved:
        rx = e.get("rx")
        if rx is None:
            continue
        where = f"Bus {e['id']} rx ({rx['shield']} -> {rx['nucleo']} -> {rx['gpio']})"
        caps = rx["capture"]
        if not caps:
            errors.append(f"{where}: no timer capture function")
            continue
        if rx_timer is not None:
            cap = next((c for c in caps if c["timer"] == rx_timer), None)
            if cap is None:
                errors.append(
                    f"{where}: no capture on {rx_timer}, other receivers use {rx_timer}"
                )
                continue
        else:
            cap = caps[0]
            rx_timer = cap["timer"]
        ch = cap["channel"]
        if ch in rx_channels:
            errors.append(
                f"{where}: {rx_timer} CH{ch} already used by Bus {rx_channels[ch]}"
            )
        rx_channels[ch] = e["id"]
        rx["tim_channel"] = ch
        rx["af"] = cap["af"]

    if errors:
        raise PinMapError("\n".join(errors))

    return resolved, rx_timer


def emit(resolved, rx_timer, board, shield, sources):
    n = len(resolved)
    ports = sorted({e[s]["port"] for e in resolved for s in SIGNALS})
    afs = sorted({e["rx"]["af"] for e in resolved})
    if len(afs) != 1:
        raise PinMapError(f"RX pins need a single AF number, got {afs}")
    af = afs[0]

    timch_to_idx = [-1, -1, -1, -1]
    for i, e in enumerate(resolved):
        timch_to_idx[e["rx"]["tim_channel"] - 1] = i

    ids = ", ".join(str(e["id"]) for e in resolved)

    L = []
    w = L.append
    w("/* Generated by cmake/gen_pins.py - DO NOT EDIT. */")
    w("/*")
    w(f" * Board : {board['board']['name']}")
    w(f" * Shield: {shield['shield']['name']} rev {shield['shield']['revision']}")
    for s in sources:
        w(f" * Source: {s}")
    w(" *")
    w(" *  Bus | term | signal | shield | nucleo  | label | gpio  | note")
    w(" *  ----+------+--------+--------+---------+-------+-------+-----------")
    for e in resolved:
        for sig in SIGNALS:
            p = e[sig]
            note = f"{rx_timer}_CH{p['tim_channel']}" if sig == "rx" else ""
            w(
                f" *  {e['id']:>3} | {e['term']:<4} | {sig:<6} | {p['shield']:<6} | "
                f"{p['nucleo']:<7} | {p['label']:<5} | {p['gpio']:<5} | {note}"
            )
    w(" */")
    w("")
    w("#ifndef BOARD_PINS_H")
    w("#define BOARD_PINS_H")
    w("")
    w("#include <stdint.h>")
    w('#include "stm32h5xx_hal.h"')
    w("")
    w(f'#define SB_SHIELD_REV     "{shield["shield"]["revision"]}"')
    w(f"#define SB_NUM_BUSES      {n}      /* buses fitted: {ids} */")
    w(f"#define SB_RX_TIM         {rx_timer}")
    w(f"#define SB_RX_TIM_AF      GPIO_AF{af}_{rx_timer}")
    w("")
    w("#define SB_GPIO_CLK_ENABLE_ALL() do { \\")
    for p in ports:
        w(f"    __HAL_RCC_GPIO{p}_CLK_ENABLE(); \\")
    w("} while (0)")
    w("")
    w("typedef struct {")
    w("    GPIO_TypeDef *port;")
    w("    uint16_t      pin;")
    w("} sb_gpio_t;")
    w("")
    w("typedef struct {")
    w("    uint8_t     id;          /* physical bus number (silkscreen)     */")
    w('    const char *term;        /* terminal block reference, e.g. "J6" */')
    w("    sb_gpio_t   rx;          /* receiver output -> timer capture      */")
    w("    sb_gpio_t   data;        /* driver input                          */")
    w("    sb_gpio_t   oe;          /* driver ~OE, active low                */")
    w("    uint32_t    rx_tim_channel;    /* TIM_CHANNEL_x                   */")
    w("} sb_bus_pins_t;")
    w("")
    w("/* Internal 0-based table, sorted by physical id. Never expose the")
    w(" * index outside the firmware: use .id. */")
    w("static const sb_bus_pins_t sb_bus_pins[SB_NUM_BUSES] = {")
    for e in resolved:

        def g(sig):
            p = e[sig]
            return f"{{ GPIO{p['port']}, GPIO_PIN_{p['num']} }}"

        ch = e["rx"]["tim_channel"]
        w(
            f'    {{ {e["id"]}, "{e["term"]}", {g("rx")}, {g("data")}, '
            f"{g('oe')}, TIM_CHANNEL_{ch} }},"
        )
    w("};")
    w("")
    w(f"/* {rx_timer} capture channel (1..4) - 1 -> table index, -1 = unused. */")
    w(
        "static const int8_t sb_timch_to_idx[4] = { "
        + ", ".join(str(x) for x in timch_to_idx)
        + " };"
    )
    w("")
    w("/* Physical bus id -> table index, or -1 if this shield has no such bus. */")
    w("static inline int sb_bus_index(unsigned id) {")
    w("    for (int i = 0; i < SB_NUM_BUSES; i++)")
    w("        if (sb_bus_pins[i].id == id) return i;")
    w("    return -1;")
    w("}")
    w("")
    w("#endif /* BOARD_PINS_H */")
    w("")
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--board", required=True)
    ap.add_argument("--mating", required=True)
    ap.add_argument("--shield", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    board, mating, shield = load(a.board), load(a.mating), load(a.shield)
    try:
        resolved, rx_timer = resolve(board, mating, shield)
        text = emit(resolved, rx_timer, board, shield, [a.board, a.mating, a.shield])
    except PinMapError as e:
        print(f"gen_pins: pin map rejected ({a.shield}):", file=sys.stderr)
        for line in str(e).splitlines():
            print(f"  - {line}", file=sys.stderr)
        sys.exit(1)

    # Write only if changed, so unchanged maps don't trigger a rebuild.
    try:
        with open(a.out) as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(a.out, "w") as f:
        f.write(text)


if __name__ == "__main__":
    main()
