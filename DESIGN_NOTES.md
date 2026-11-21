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

**Measured at 250000 (milestone 2, 2026-11-21):** 1000 frames received
(30,000 bytes) with 0 framing errors, 0 bad tails, 0 USART overruns and
0 ring-buffer drops. Frame rate read 11 Hz against the datasheet's
nominal 10. So the −2.34% link works on this particular radar and board at
room temperature. A clean run of 30,000 bytes only shows the byte error rate
is below about 1×10⁻⁴ (95% bound, 3/30,000). It doesn't show any margin
against temperature or supply drift in the Uno's clock, or against a
different radar unit's oscillator.

Chosen run-time baud: 38400 (radar index 0x0003).

**Switch performed 2026-11-21** with the `RADAR_SETUP=1` build. Enable config,
set baud and restart were each ACKed on the first run, and data frames
were confirmed at 38400.

**Measured at 38400:** 1000 frames with 0 framing errors, 0 bad tails,
0 overruns and 0 drops, the same as at 250000. However, the frame rate read
**6 Hz**, versus 11 Hz at 250000. All AVR-side counters are zero, so no
frames are being lost on the AVR side. The radar itself is sending fewer
frames. _Under investigation:_ check whether the rate depends on targets
being present, or on the radar's baud setting.

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

A full frame is 320 × 240 × 2 B = 153,600 B. The ATmega328P has 2,048 B of
SRAM, so a framebuffer is impossible, as is even a single 16-bit line
buffer of any real height. Every primitive writes straight to the
ILI9341's own frame memory through an address window (CASET/PASET +
RAMWR [ILI9341 §8.2.20–8.2.22]). Erasing an object means drawing it again in
the background color.

Expected full-screen fill time at 4 MHz SPI:

```
SPI byte time   = 8 bits / 4 MHz = 2 us (32 CPU cycles)
+ loop/poll overhead               ~10-15 cycles per byte
per pixel (2 B)                    ~90 cycles = 5.6 us
76,800 px                          ~0.43 s
```

Milestone 1 measures this with Timer1 and prints it on the screen.
**Measured: 537 ms**, about 25% over the estimate:

```
537 ms / 153,600 B = 3.5 us per byte = 56 cycles
SPI shift = 32 cycles, so call + SPIF polling + 32-bit loop counter ~ 24 cycles
```

An unrolled loop that loads SPDR as soon as SPIF sets could get close to the
2 us/byte floor (~0.31 s). That isn't worth doing, because the radar view
never fills the whole screen during normal operation. This is why the radar view must
redraw only what changed: a ping is 4×4 px and a label is about 30×8 px,
which take well under 1 ms each.

Text is drawn opaque: each 6×8 character cell is one address window with
the background included. New text overwrites old text in place with no
separate clear, and it's about 30× faster than plotting pixels one at a time.

Font: a 5×7 font in printable ASCII, drawn by hand as ASCII art in
`tools/font5x7.txt`. `tools/gen_font.py` converts it to column bytes
(bit 0 = top row) in `src/font5x7.c`, 95 × 5 = 475 B of PROGMEM. The
generated file is committed, so building doesn't need Python. A library
font table was not used, to meet the "no copied library code" rule.

## ILI9341 init sequence

Kept to the minimum the datasheet requires:

1. Hardware reset: RESX low ≥ 10 µs, then wait 120 ms (tRT) [§15.4].
2. Software reset 0x01, wait 120 ms [§8.2.2]. This also gives a warm AVR
   reset the same starting state.
3. Sleep Out 0x11, wait 120 ms [§8.2.12].
4. COLMOD 0x3A = 0x55, i.e. 16 bpp. The default 0x66 is 18 bpp and would
   cost 3 bytes per pixel [§8.2.33].
5. MADCTL 0x36 = MV | BGR (0x28): landscape, BGR filter [§8.2.29].
6. Display ON 0x29 [§8.2.19].

Power control, VCOM and gamma registers are left at the defaults the panel
loads from NV memory at reset. Vendor init sequences set dozens of these
registers, but copying one would break the "no copied library code" rule,
and the datasheet doesn't require them. If the image looks washed out,
those registers (§8.3) are the next thing to tune.

## SPI clock speeds

Both devices use SPI mode 0, MSB first: the ILI9341 samples on the rising SCL
edge [§7.1.8], and the XPT2046 latches DIN on the rising edge and shifts DOUT
on the falling edge.

| Device | Divider | SCK | Limit |
|---|---|---|---|
| ILI9341 | fosc/4 (SPR=00, SPI2X=0) | 4 MHz | write cycle ≥ 100 ns, i.e. 10 MHz [ILI9341 §19.3.4]; capped at 4 MHz for the TXS0108E |
| XPT2046 | fosc/16 (SPR=01, SPI2X=0) | 1 MHz | tCH, tCL ≥ 200 ns, i.e. 2.5 MHz [XPT2046 Table 9] |

Dividers are from [M328P Table 19-5].

The XPT2046's sample (acquisition) phase is 3 DCLK cycles. At 1 MHz that is
3 µs, which gives the resistive panel more settling time than 2 MHz would.
Touch reads are only a few bytes each, so the slower clock costs nothing
noticeable.

PB2 is both the display CS and the SPI SS pin. It is configured as an
output before SPI is enabled, because an SS input pulled low would
silently switch the SPI back to slave mode [M328P §19.3.2]. Touch CS (PD7)
is driven high before any SPI traffic, so the XPT2046 never interprets
display data as commands.

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
