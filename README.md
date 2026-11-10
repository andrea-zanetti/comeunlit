# Comelit SimpleBus blind bridge

Firmware and hardware for an Ethernet bridge to Comelit SimpleBus blind
modules (dual relay module 20004606), built on an ST NUCLEO-H563ZI with a
small interface shield.

The bridge is a **stateless actuator** with a signed HTTP API: a home
automation system asks it to read a blind, move it up/down/stop, or move a
group in sync, and can follow wall-switch presses live. The bridge owns the
bus timing and the module's rules (stop, reversal); it keeps no
configuration and no state.

> Not affiliated with or endorsed by Comelit. The protocol was
> reverse-engineered from bus captures for interoperability with equipment
> the author owns. Comelit and SimpleBus are trademarks of their owner.

## Why this might be useful to you

Even if you never control a blind, this repository gives you a working
starting point for SimpleBus, instead of a blank page:

* **A frame encoder and decoder** (`Core/Src/sb_frame.c`): preambles,
  UART-style bytes, checksums, and the 59-symbol shape modules use. Plain C,
  no hardware dependencies, covered by host tests.
* **A receiver and a transmitter with real timing:** edge capture to
  symbols (`Core/Src/simplebus_rx.c`), and hardware-timed transmission
  (`Core/Src/simplebus_tx.c`, `Core/Src/sb_tx_table.c`).
* **The hardware to get on the bus:** the interface shield
  (`hw/shield_rev1/`, KiCad 10, self-contained), with its known rev 1
  erratum documented.
* **Tools to explore the protocol:** a live stream of every frame on every
  bus and raw transmit (`tools/sbctl.py frames`, `tools/sbctl.py tx`), plus a
  generator for an HP 8175A test pattern (`tools/bench/`).
* **What is known about the protocol, and how it was found out:**
  [docs/simplebus_protocol_reference.md](docs/simplebus_protocol_reference.md)
  marks every statement by its source (scope, bridge or programmer) and
  lists the open questions.

## How it was made

The hardware was designed, built and debugged by the author. The firmware
was developed with the help of an AI assistant (Anthropic's Claude), under
the author's direction and review.

It is not untested generated code: the bus behaviour and timings described
here were checked on the bench with real test and measurement instruments (an Agilent MSO6034A
oscilloscope and an HP 8175A data generator) and on a real SimpleBus
installation, with real modules and wall switches. For example, the
module's 1.5 ms turnaround and the bit timings come from scope captures;
the extra pulse that made modules answer 33 ms late was found by comparing
two frame versions back to back on the live bus; and the hardware-timed
transmitter was checked by receiving 622 of 622 frames from the data
generator on one bus while the bridge transmitted on another.

## Status

| Part | State |
|------|-------|
| Bus receive / transmit, 3 buses | working on a live installation |
| Blind control (read, up, down, stop, reversal, batch) | working |
| Signed API, streams, raw admin access | working (firmware 0.1.0) |
| Shield | rev 1 built, **bus 1 unusable** (see below); rev 2 planned |

## Hardware

### Board

* **ST NUCLEO-H563ZI** (STM32H563ZI, Cortex-M33 at 250 MHz, Ethernet).
* **Power and clock come from the on-board ST-LINK**: the board is powered
  through the ST-LINK USB connector, and the MCU runs from the ST-LINK's
  8 MHz clock output (HSE bypass, PLL to 250 MHz). This is deliberate: do
  not "fix" it to HSI.
* In the installation, a PoE splitter provides both the Ethernet link and
  the 5 V USB supply; its cables are long enough to reach the Ethernet and
  ST-LINK USB ports on opposite sides of the board.
* Console: ST-LINK virtual COM port, 115200 8N1.

### Shield rev 1

One SimpleBus line per bus: an SN74LVC125A receiver into a TIM1 capture
input, and an SN74HCT125 driver with active-low output enable (10 kΩ
pull-ups keep the drivers tri-stated at boot). Bus *n* is on terminal
J(*n*+4).

| Bus | Terminal | RX (capture) | TX data | TX enable | State |
|-----|----------|--------------|---------|-----------|-------|
| 1 | J5 | PG12 (no capture) | — | — | **unusable** |
| 2 | J6 | PE9  TIM1_CH1 | PG14 | PC0 | working |
| 3 | J7 | PE11 TIM1_CH2 | PB6  | PC3 | working |
| 4 | J8 | PE14 TIM1_CH4 | PB7  | PB1 | working |

**Rev 1 erratum: bus 1 is fubar.** Its receiver is routed to D7 / PG12,
which has no timer capture channel, so the bridge cannot receive on it.
Rather than bodge the board, rev 1 runs on buses 2–4: three buses are
plenty for a typical flat (two in use here, one spare). Rev 2 will move
the bus 1 receiver to a capture-capable pin; its provisional map is
`hw/pins/shield_rev2.toml`.

The pin assignment is not hand-written anywhere in the code: it is
generated at build time from the TOML files in `hw/pins/` (board, connector
mating, shield revision) by `cmake/gen_pins.py`, which also checks that
every receiver lands on a TIM1 capture channel. The shield's KiCad files
live in `hw/shield_rev1/`.

## Security model

The bridge serves only `/api/v1/`, and **every request, response and stream
event is signed** with HMAC-SHA256 (hardware HASH unit) using a 32-byte key
compiled into the firmware. A per-boot random session (hardware RNG) and a
64-entry counter window reject replays. There is no TLS: the API gives
integrity and authenticity, not confidentiality. Put the bridge on a
network segment where only the home automation host can reach it.

Details: [docs/API.md](docs/API.md), section 3.

## Building

Requirements: `arm-none-eabi-gcc`, CMake ≥ 3.20, Python ≥ 3.11 (`tomllib`;
older Pythons need `pip install tomli`), and STM32CubeProgrammer's
`STM32_Programmer_CLI` for the `flash` target.

```sh
# 1. API key (never commit secrets.toml; it is gitignored)
printf '[auth]\nkey = "%s"\n' \
  "$(python3 -c 'import secrets; print(secrets.token_hex(32))')" > secrets.toml

# 2. Configure (shield rev 1 is the default) and build
cmake -B build -DSHIELD_REV=1
cmake --build build

# 3. Flash over the ST-LINK
cmake --build build --target flash
```

The build stops at configure time if `secrets.toml` is missing or its key is
malformed. The project is plain CMake and is **not** managed by STM32CubeMX:
there is no `.ioc`, and `main.c`, the HAL configuration and the FreeRTOS
configuration are ordinary source files.

**MAC address.** By default each board derives its Ethernet MAC from the
chip's 96-bit unique ID: a locally administered address (`x2:`, `x6:`, `xA:`
or `xE:` first octet), different on every board and stable across reflashes.
It is printed at boot (`[NET] MAC ...`). To pin a fixed one instead, for
example to keep an existing DHCP reservation:

```sh
cmake -B build -DSB_ETH_MAC=aa:bb:cc:dd:ee:ff
```

Configure fails if the value is malformed or a multicast address. Pass
`-DSB_ETH_MAC=` (empty) to go back to the derived address.

### First boot

On the console:

```
=== SimpleBus Bridge ===
[TX] shield rev 1, buses: 2(J6) 3(J7) 4(J8), LBT on
MAIN: starting kernel
[NET] MAC 02:1a:2b:3c:4d:5e (from chip UID)
[NET] Link up, starting DHCP...
[NET] DHCP address: 10.0.0.42
[CRYPTO] HASH HMAC-SHA256 + RNG OK (4 HMAC vectors, RNG); boot random ...
[API] session ...
[HTTP] Listening on port 80 (API v1 only)
```

The bridge uses DHCP and has no fallback address: if no lease arrives
within a minute it logs `[NET] No DHCP lease yet (still trying)`, keeps
trying, and answers as soon as it gets one. A reservation for the MAC above
gives it a stable address.

The `[CRYPTO]` line is the boot self-test of the HASH unit (RFC 4231 test
vectors) and the RNG. If it fails, the API refuses every request with
`503 crypto` and the console says which test failed.

## Using it

`tools/sbctl.py` is a signing command-line client (standard library only):

```sh
export SB_HOST=10.0.0.42
export SB_SECRETS=$PWD/secrets.toml

tools/sbctl.py health
tools/sbctl.py scan 2 1-32          # find modules on bus 2
tools/sbctl.py get 2:6
tools/sbctl.py up 2:6               # also: down, stop
tools/sbctl.py batch down 2:3 2:4 2:5
tools/sbctl.py events               # wall-switch presses and acks, live
tools/sbctl.py frames               # every frame on every bus
tools/sbctl.py tx 2 "21 06 00 00"   # raw frame (checksum appended)
```

The signing functions at the top of `sbctl.py` are a reference for writing
a client in any language.

## Repository layout

```
Core/            application (bus, blind logic, API, auth, streams), HAL config
LWIP/            network glue and the HTTP server
Drivers/         ST HAL v1.6.0, CMSIS, BSP (vendored)
Middlewares/     FreeRTOS, lwIP (vendored)
hw/pins/         pin maps (TOML) -> generated board_pins.h
hw/shield_rev1/  shield KiCad files
cmake/           toolchain file, pin-map and secret generators
tools/sbctl.py   signed CLI client
tools/bench/     bench helpers (HP 8175A test pattern generator)
tests/host/      host tests and the end-to-end fake bridge
docs/            API and protocol reference
```

## Tests

The bus-independent parts (frame codec, receiver, blind logic, API,
authentication, streams, MAC derivation, the transmitter's DMA table) build and run on a PC; OpenSSL stands in for the
HASH unit and a simulated bus for the modules.

```sh
make -C tests/host        # C tests
make -C tests/host e2e    # sbctl.py against the firmware's API on a socket
```

## Documentation

* [docs/API.md](docs/API.md): the HTTP API, signing, endpoints, errors.
* [docs/simplebus_protocol_reference.md](docs/simplebus_protocol_reference.md):
  the bus protocol as reverse-engineered, with the annotated scope capture.

## Known limitations

* Shield rev 1: bus 1 unusable (above).
* Modules do not announce the end of travel; a client that needs to know a
  blind has stopped must poll it.
* No automatic retries: a press is not idempotent, so a missing ack is
  reported and the client decides.
* Open protocol questions are listed at the end of the protocol reference.

## Licence

This project's own code and its documentation and hardware use different
licences, and the vendored or ST-derived code keeps its own. In short: any
use except commercial use. This is a summary, not legal advice; the licence
texts decide.

| What | Licence |
|------|---------|
| Firmware written for this project, `cmake/`, `tools/`, `tests/` | [PolyForm Noncommercial 1.0.0](LICENSE) |
| `docs/`, `hw/` (except `hw/shield_rev1/lib/`) | [CC BY-NC-SA 4.0](LICENSE-docs-hw) |
| `hw/shield_rev1/lib/` (symbols, footprints, 3D models from SnapEDA and KiCad) | CC BY-SA 4.0, see [its NOTICE](hw/shield_rev1/lib/NOTICE.md) |
| The zmonad name and logo (the `LOGO` footprint on the board) | Not licensed: all rights reserved |
| Files with an STMicroelectronics copyright header (in `Core/` and `LWIP/`) | ST's terms, see below |
| `Drivers/STM32H5xx_HAL_Driver`, `Drivers/BSP` | BSD 3-Clause (STMicroelectronics) |
| `Drivers/CMSIS` | Apache 2.0 (Arm, STMicroelectronics) |
| `Middlewares/Third_Party/FreeRTOS` | MIT (Amazon) |
| `Middlewares/Third_Party/LwIP` | BSD 3-Clause (Swedish Institute of Computer Science) |

**ST-derived files.** Some files in `Core/` and `LWIP/` were generated by
STM32CubeMX or adapted from STM32CubeH5; they keep their STMicroelectronics
header and stay under ST's terms, including the changes made here. The
network glue (`LWIP/Target/ethernetif.[ch]`, `LWIP/App/app_ethernet.[ch]`)
comes from ST's `LwIP_HTTP_Server_Socket_RTOS` application; STM32CubeH5
licenses its application projects under ST's SLA0044, which allows
redistribution in source form if the notice is kept, requires the code to
run only on ST devices, and does not allow placing it under open-source
terms. None of this limits running the bridge on its intended STM32H563.

**zmonad logo.** The board carries the author's zmonad logo. It is not
covered by any licence here: unmodified copies of the repository may keep
it, but modified or derived boards must remove the `LOGO` footprint.

PolyForm Noncommercial is not an open-source licence in the OSI sense: it
allows personal, hobby, research, educational and other noncommercial use,
and does not allow commercial use without separate permission.
