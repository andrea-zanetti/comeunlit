#!/usr/bin/env python3
"""Generate an HP 8175A ibtest input that plays a SimpleBus frame, the way a
module sends it, followed by an idle gap: for bench tests of the bridge's
receiver (e.g. receiving on one bus while the bridge transmits on another).

The pattern is one 8175A step per pulse or gap (CHD levels, CHT durations):

    level 0 = bus LOW pulse, level 1 = bus HIGH
    pulse 187 us; gap 187 us (bit 1) or 279 us (bit 0); module timing as
    measured on the scope (docs/simplebus_protocol_reference.md, section 2)

A frame is 59 symbols + a closing pulse = 60 pulses; the last step is the
idle gap that separates repeats. 59 * 2 + 2 = 120 steps (0..119).

Output follows the structure of the ibtest script used on the bench
(device 30, timeout 14). Usage:

    tools/bench/hp8175a_pattern.py > hp8175a_frame.txt
    tools/bench/hp8175a_pattern.py --frame "00 0A 01 01" --idle-us 70000
    tools/bench/hp8175a_pattern.py --cycle-mode 0    # one frame only
    ibtest < hp8175a_frame.txt
"""

import argparse
import sys

PREAMBLE = "0101010101"  # STD class
SEED = 0x55
PULSE_US = 187
GAP1_US = 187
GAP0_US = 279


def encode(data: bytes) -> str:
    """Symbols as a module sends them: preamble, bytes + checksum UART-style
    (start 0, 8 data bits LSB first, stop 1), final stop bit left out."""
    chk = (SEED + sum(data)) & 0xFF
    s = PREAMBLE
    for v in list(data) + [chk]:
        s += "0" + "".join("1" if v >> i & 1 else "0" for i in range(8)) + "1"
    return s[:-1]


def pattern(symbols: str, idle_us: int):
    levels, times = [], []
    for c in symbols:
        levels += [0, 1]
        times += [PULSE_US, GAP1_US if c == "1" else GAP0_US]
    levels += [0, 1]  # closing pulse, then the idle gap
    times += [PULSE_US, idle_us]
    return levels, times


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--frame", default="00 0A 01 01",
                    help='4 bytes, checksum appended (default "00 0A 01 01")')
    ap.add_argument("--idle-us", type=int, default=70000,
                    help="idle gap after each frame, us (default 70000)")
    ap.add_argument("--cycle-mode", default="1",
                    help="CYM value: 1 = auto cycle, loops until STOP "
                         "(default); 0 = single cycle")
    ap.add_argument("--device", default="30", help="ibtest device (default 30)")
    ap.add_argument("--timeout", default="14", help="ibtest timeout (default 14)")
    a = ap.parse_args(argv)

    data = bytes.fromhex(a.frame.replace(" ", ""))
    if len(data) != 4:
        ap.error("--frame must be 4 bytes")
    sym = encode(data)
    levels, times = pattern(sym, a.idle_us)
    last = len(levels) - 1

    period_ms = sum(times) / 1000
    print(f"frame {a.frame} + checksum: {len(sym)} symbols, "
          f"{levels.count(0)} pulses, {len(levels)} steps, "
          f"period {period_ms:.1f} ms", file=sys.stderr)

    cmds = [
        "RST",
        "DM0;DUR1;IFM( B ),,,1",
        "DM1;CFM ( B )",
        "TSA0;CHD0," + ",".join(map(str, levels)),
        "TSA0;CHT " + ",".join(f"{t}us" for t in times),
        "TSA 0",
        "HILP 0,5",
        "POD 1",
        f"PM0;CD;CR12;(0{last:03d})",
        f"CYM {a.cycle_mode}",
        "UP",
    ]
    out = ["", "d", a.device, "t", a.timeout]
    for c in cmds:
        out += ["w", c]
    out += ["m", "q", ""]
    sys.stdout.write("\n".join(out))


if __name__ == "__main__":
    main()
