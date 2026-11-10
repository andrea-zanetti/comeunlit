# Comelit SimpleBus Protocol Reference

**Version 3.1** (2026-11-08). Adds the behaviour verified with the bridge
firmware on a live installation: button-press semantics, announcements,
stop and reversal, repeat patterns and bridge-side timing. 3.1: frames must
end after 60 pulses; a 61st pulse delays the module's reply by ~33 ms
(section 2). The bridge's own
HTTP API is specified separately in [API.md](API.md).

How to read this document: statements are marked by where they come from.

* **Scope**: measured on the oscilloscope (Agilent MSO6034A) while the
  Comelit programmer talked to a module.
* **Bridge**: observed with the bridge's frame stream and command path, 2026-11.
* **Programmer**: seen in captures of the Comelit programmer / master;
  not used or re-verified by the bridge.

![Programmer DOWN command and the module's response, decoded down to
individual PLE symbols](img/simplebus_command_down.png)

*Figure: a DOWN press from the Comelit programmer to module 6 and the
module's answer (capture `maybe_cmd.bin`, 40 MSa/s). Request
`10 06 02 02 6F`, response `00 06 02 02 5F`; 1.5 ms between the end of the
request and the start of the response. Middle: preamble and the first
bytes; bottom: byte 2 (`0x02`) symbol by symbol.*

## 1. Physical layer

### Encoding: Pulse Length Encoding (PLE)

* Line idles HIGH, active-low signalling.
* Every symbol is a fixed LOW pulse followed by a HIGH gap whose length
  carries the bit:

| Part        | Duration   | Meaning       |
|-------------|------------|---------------|
| LOW pulse   | **187 µs** | symbol marker |
| short HIGH  | **187 µs** | bit `1` (mark), symbol 374 µs  |
| long HIGH   | **279 µs** | bit `0` (space), symbol 466 µs |

* About 2400 baud on average.
* Decoding threshold used by the bridge: LOW-to-LOW interval < 420 µs is a
  `1`, otherwise `0`. A gap over 1 ms ends a frame.

### Electrical (Scope)

Measured on the 5 V logic side of the programmer's bus interface:

* Logic levels 0 / 5 V, idle HIGH.
* Devices drive both HIGH and LOW (push-pull), with a weak pull-up
  (about 5 kΩ) holding the line HIGH when nobody drives it.

The bridge connects to the same logic-level signal through its shield
(SN74HCT125 driver with active-low output enable, SN74LVC125A receiver).

## 2. Frame structure

1. **Preamble**: 10 symbols; identifies the protocol class and the checksum
   seed (section 3).
2. **Data**: 5 bytes, each as a UART-style 10-bit group:
   start bit `0`, 8 data bits LSB first, stop bit `1`.

That is 60 symbols, about 26 ms; on the wire the last one (the final stop
bit) is not sent as a symbol (below).

**Pulses vs symbols.** A symbol's value is the HIGH gap after its LOW pulse,
so the last symbol has no closing edge:

* Modules send **60 LOW pulses = 59 measurable symbols**; the final stop
  bit is implied by the idle HIGH after the last pulse (Scope, Bridge).
* A transmitter must do the same: 59 symbols plus a closing LOW pulse,
  60 pulses in all. The bridge does this since firmware 0.1.0.
* A **61st pulse** (all 60 symbols plus the closing pulse) is still
  accepted, but the module then replies about 33 ms late: end of request to
  end of reply goes from 28 ms to 60 ms, the same for polls and presses
  (Bridge, 2026-11-08, same module, both versions back to back). The likely
  reading is that the module takes the extra pulse as the start of another
  frame and answers only after its own end-of-frame timeout.
* A receiver therefore decodes from 59 symbols: the 5th byte (checksum)
  needs only its start bit and 8 data bits.

## 3. Protocol classes

| Preamble     | Seed   | Class | Direction       | Purpose                         |
|--------------|--------|-------|-----------------|---------------------------------|
| `0101010101` | `0x55` | STD   | both            | poll, command, reply, announce  |
| `0111011101` | `0x77` | EXT   | master → device | extended register read request  |
| `0110011001` | `0x33` | RSP   | device → master | extended register read response |

## 4. Message format (5 bytes)

| Byte | Name     | Description                                   |
|------|----------|-----------------------------------------------|
| B0   | Type     | message type (section 5)                      |
| B1   | Address  | device address (1–254)                        |
| B2   | In       | button / input, or register id (EXT/RSP)      |
| B3   | Out      | relay / output state, or register value       |
| B4   | Checksum | `(B0 + B1 + B2 + B3 + seed) & 0xFF`           |

## 5. Standard protocol (STD, seed `0x55`)

### Message types

| Type   | Name        | Direction        | Meaning |
|--------|-------------|------------------|---------|
| `0x21` | POLL        | master → device  | status query, B2 = B3 = 0 |
| `0x01` | POLL_REPLY  | device → master  | B3 = current relay state |
| `0x10` | PRESS       | master → device  | press a button (section 5.2) |
| `0x00` | ANNOUNCE    | device → all     | a button was pressed: B2 = button, B3 = new relay state (section 5.3) |

### 5.1 Poll

```
master:  21 aa 00 00 cc      POLL
device:  01 aa 00 ss cc      POLL_REPLY, ss = relay state
```

The Comelit master polls each known device about every 2 s (Programmer).
The bridge polls only on request (Bridge).

### 5.2 Button press (Bridge, dual relay blind module 20004606)

A `0x10` frame is a press of one of the module's two buttons, exactly as if
the wall switch had been pressed:

| Frame          | Effect |
|----------------|--------|
| `10 aa 01 01`  | press button 1 (UP on this installation) |
| `10 aa 02 02`  | press button 2 (DOWN on this installation) |
| `10 aa 00 00`  | nothing: there is **no stop frame** |

Which button raises the blind depends on the wiring; 1 = UP, 2 = DOWN on
every module tested here.

Relay state (B3 of replies and announcements):

| Value | State |
|-------|-------|
| `00`  | idle  |
| `01`  | moving up (relay 1)   |
| `02`  | moving down (relay 2) |

How the module reacts to a press:

| Current state | Press                          | Result                    |
|---------------|--------------------------------|---------------------------|
| idle          | either button                  | moves in that button's direction |
| moving        | button of the current direction | nothing changes          |
| moving        | the other button               | **stops** (state `00`)    |

So:

* **STOP** = press the button opposite to the current motion.
* **Reverse** = stop (opposite press), pause, press again. The bridge
  pauses 500 ms to rest the motor.
* The module stops by itself at the end of its travel timer and **does not
  announce it**; only a poll shows the change.

Programmer pattern (Programmer), for reference: the programmer sends a press
and then a second frame with the output byte cleared, e.g.
`10 06 02 02` then `10 06 02 00`; the module answers `00 06 02 02` to both.
The bridge sends single presses; the second frame is not needed.

### 5.3 Announcements (`0x00`)

A module sends `00 aa bb ss` whenever one of its buttons is pressed, by
whatever means:

* **after a bridge press**: exactly once, as the acknowledgement
  (Bridge, single commands, reversals and a 3-module batch);
* **after a wall-switch press**: **twice**, 131–252 ms apart, with no master
  frame before it (Bridge).

B2 is the button pressed, B3 the relay state after the press. Examples
captured from wall switches on module 6:

```
00 06 01 01 5D   UP pressed, now moving up      (sent twice)
00 06 02 00 5D   DOWN pressed while going up -> stopped (sent twice)
00 06 02 02 5F   DOWN pressed, now moving down  (sent twice)
```

A press frame and its acknowledgement are told apart from a wall-switch
press only by timing: the acknowledgement follows the bridge's own `0x10`
frame to the same address with the same button.

## 6. Extended protocol (EXT / RSP, seeds `0x77` / `0x33`) (Programmer)

Used by the Comelit programmer during discovery and configuration to read
device identification registers. Not used by the bridge; the bridge's raw
frame stream shows these frames decoded (`proto` EXT / RSP).

### Register read

```
master:  00 aa rr 00 cc      EXT preamble, seed 0x77
device:  00 aa rr vv cc      RSP preamble, seed 0x33
```

### Known registers

| Register | Purpose            | Values observed                       |
|----------|--------------------|---------------------------------------|
| `0x2A`   | device type (high) | `0x0B` (input module), `0x25` (relay) |
| `0x2B`   | device type (low)  | `0x3D` (input module), `0x3E` (relay) |

### Known device types

| Reg `0x2A` | Reg `0x2B` | Model    | Description                |
|------------|------------|----------|----------------------------|
| `0x0B`     | `0x3D`     | 20046501 | input module               |
| `0x25`     | `0x3E`     | 20004606 | dual relay module (blinds) |

## 7. Discovery (Programmer)

The programmer scans addresses 1–254:

1. Poll address N with `21 N 00 00` (STD).
2. No answer after 3 tries (about 1 s apart): next address.
3. On `01 N 00 00`: read register `0x2A` (`00 N 2A 00`, EXT) and `0x2B`
   (`00 N 2B 00`, EXT); the device answers with RSP frames.
4. Next address.

Frames during discovery are about 30 ms apart.

The bridge discovers modules with plain polls only (`sbctl scan`, one poll
per address, no retry).

## 8. Checksum

All classes use the same sum with a class-specific seed:

```
checksum = (B0 + B1 + B2 + B3 + SEED) & 0xFF
SEED     = 0x55 (STD), 0x77 (EXT), 0x33 (RSP)
```

Example: `10 06 02 02` → `0x10 + 0x06 + 0x02 + 0x02 + 0x55 = 0x6F`.

## 9. Timing

| Parameter | Value | Source |
|-----------|-------|--------|
| Frame duration | about 26 ms | Scope |
| End of request → start of response | 1.5 ms | Scope (programmer → module) |
| End of bridge frame → reply published by the bridge's receiver | 28 ms with 60 pulses; 60–62 ms with 61 | Bridge (see note) |
| Repeat of a wall-switch announcement | 131–252 ms after the first | Bridge |
| Master poll interval | about 2 s | Programmer |
| Discovery frame spacing | about 30 ms | Programmer |
| Discovery poll retries | 3, about 1 s apart | Programmer |

Bridge settings that work reliably with the modules (firmware 0.1.0):

| Setting | Value |
|---------|-------|
| Bit timing sent (TIM7 + DMA, 93 µs grid) | LOW 186 µs, HIGH 186 µs (`1`) / 279 µs (`0`) |
| Listen before talk | line idle 2 ms before sending, give up after 50 ms |
| Reply timeout | 150 ms |
| Quiet time after a reply (or timeout) before the next frame | 40 ms |
| Pause between the stop and the second press of a reversal | 500 ms |

Note on the bridge figure: it is measured from the bridge's timestamp at
the end of its own transmission to the timestamp when the receiver
publishes the complete reply, so it includes the ~26 ms reply itself and up
to ~2 ms of receiver processing. With 60 pulses that leaves about 1.5 ms of
turnaround, matching the scope. The 60–62 ms seen earlier came from the
61st pulse (section 2).

Note on bridge timestamps: the bridge sends with a timer and DMA, with
interrupts enabled, so its timestamps are continuous and it keeps receiving
on the other buses while it transmits. (An earlier bit-banged transmitter
masked interrupts for each ~26 ms frame; captures made with it read about
26 ms short across a bridge transmission, e.g. ~43 ms from a reply to the
next bridge frame instead of 40 + 2 + 26 ms.)

## 10. Open questions

* Whether modules ever repeat their acknowledgement to a bridge press (not
  seen so far; a repeat colliding with the bridge's next frame in a batch
  would show as garbled frames).
* Meaning of other register ids in the EXT/RSP class.
* Behaviour of the input module (20046501) on the STD class beyond polls.
