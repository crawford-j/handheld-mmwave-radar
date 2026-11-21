# Design Notes

Running log of design decisions, calculations, and alternatives considered
but not chosen. Feeds the final design report.

## LD2450 protocol check

Checked against the HLK-LD2450 Serial Communication Protocol V1.03 (2023-10-17).

| Item | Assumed | Datasheet | Result |
|---|---|---|---|
| Default UART | 256000 8N1 | 256000, 1 stop, no parity [§1.2.1, §2] | Confirmed |
| Byte order | — | Little-endian [§2.1.1] | — |
| Data frame | `AA FF 03 00` + 3×8 B + `55 CC` | Same, 30 bytes total, 10 frames/s [§2.3 Table 9] | Confirmed |
| X, Y, speed | int16, top bit set = positive | Bit 15 = 1 means positive, 0 means negative; other 15 bits are magnitude [§2.3 Table 10] | Confirmed (sign-magnitude, inverted sign bit) |
| 4th field | "distance resolution" uint16 | Size of one distance gate in mm, **not** the distance to the target [Table 10] | Distance must be computed as √(x²+y²) |
| Empty target | All zeros | Absent targets are all 0x00 [§2.3 example] | Confirmed |
| Command frame | `FD FC FB FA` … `04 03 02 01` | Header, 2-byte length, cmd word + value, tail [§2.1.2 Table 2] | Confirmed |
| ACK word | — | Command word OR 0x0100, then 2-byte status (0 = OK) [Table 5] | — |
| Enable config | — | 0x00FF, value 0x0001 [§2.2.1] | Confirmed |
| End config | — | 0x00FE [§2.2.2] | Confirmed |
| Set baud | — | 0x00A1 + 2-byte index; persists, applies after restart [§2.2.7] | Confirmed |
| Baud index 38400 | — | 0x0003 [Table 6] | Confirmed |
| Restart | — | 0x00A3 [§2.2.9] | Confirmed |

Decoding, worked from the datasheet example `0E 03 B1 86 10 00 40 01`:

```
X     raw 0x030E = 782,   bit15 = 0 -> -782 mm
Y     raw 0x86B1 = 34481, bit15 = 1 -> 34481 - 32768 = +1713 mm
speed raw 0x0010 = 16,    bit15 = 0 -> -16 cm/s
res   raw 0x0140 = 320 mm (the module manual prints 360, a typo)
```

Other notes:
- The module manual gives a ±60° azimuth, ±35° elevation and a maximum range of 6 m.
- Targets are tracked in fixed slots, so slot *n* stays the same person from frame to frame.
- Hi-Link's own parsing example applies a −0x8000 offset to Y in both branches. Y is always positive, since targets are always in front of the antenna, so the example only works for that reason. The firmware decodes all three signed fields the same way.

## Radar UART baud rate

The 256000 default can't be generated exactly from 16 MHz:

```
Normal speed (U2X0=0): baud = f/(16(UBRR+1))   [M328P §20.3 Table 20-1]
  UBRR = 3 -> 16e6/(16*4) = 250000   error vs 256000 = -2.34%
Double speed (U2X0=1): baud = f/(8(UBRR+1))
  UBRR = 7 -> 16e6/(8*8)  = 250000   error = -2.34%
```

Receiver tolerance for 8 data bits, no parity [M328P §20.8, Tables 20-2/20-3]:

| Mode | Theoretical max | Recommended max |
|---|---|---|
| U2X0 = 0 | +4.58 / −4.54 % | ±2.0 % |
| U2X0 = 1 | +3.90 / −4.00 % | ±1.5 % |

At −2.34%, either mode is inside the theoretical limit but outside the
recommended one. The recommended limit leaves room for the transmitter's
clock error and noise; the Uno's 16 MHz clock source has its own tolerance.
**Normal speed mode (U2X0=0, UBRR=3) has more margin, so the one-time setup
uses it instead of U2X with UBRR=7.**

Chosen run-time baud: 38400 (radar index 0x0003).

```
U2X0=0, UBRR=25 -> 16e6/(16*26) = 38461.5   error = +0.16%   [Table 20-7]
U2X0=1, UBRR=51 -> 16e6/(8*52)  = 38461.5   error = +0.16%
```

U2X0=0 is used again for the wider tolerance. Load: 30 bytes × 10 bits ×
10 frames/s = 3000 bit/s, about 8% of the link.

Alternatives considered:
- Run at 250000 permanently. Rejected: there would be no error margin for the
  whole life of the device.
- Set the baud from a PC with a USB-UART adapter at exactly 256000. This is the
  zero-risk option, if an adapter is available.
- One-time AVR setup routine at 250000 (build option `RADAR_SETUP=1`). If a
  command is garbled, the radar sends no ACK and nothing changes, so it can
  simply be retried. The header, length and tail make it very unlikely that a
  corrupted frame is accepted as a different valid command.

## Display: no framebuffer, partial redraw

_TBD_

## SPI clock speeds

_TBD_

## Target smoothing

_TBD_

## Alert zone hysteresis

_TBD_

## Buzzer tone (Timer2)

_TBD_

## Touch calibration

_TBD_

## EEPROM settings format

_TBD_

## Timer assignments

_TBD_

## Memory budget

_TBD_

## Things that didn't work

_None yet._
