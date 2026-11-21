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
frames. A hand test of pointing the radar at a seated person and panning,
then pointing it at an empty wall, showed 6 Hz both ways, so the rate does
not depend on targets being present.

**Switched to 57600 (2026-11-21)** with `RADAR_SETUP_BAUD=57600`
(U2X0 = 1, UBRR0 = 34, −0.79%). The setup program measures the frame rate
over 3 s before and after the switch: **5 Hz at 38400 and 5 Hz at 57600.**
So between 38400 and 57600 the frame rate doesn't depend on the baud
rate, yet it was 11 Hz at the factory 256000. That leaves two explanations:
either the radar's firmware only sends at full rate at its native baud,
or something else changed when the radar was first reconfigured and
restarted. Restoring 256000 would show which one it is.

**Decision (2026-11-21): stay at 57600, about 5 Hz.** Frame rate doesn't
affect accuracy: every frame carries the same ~320 mm gate resolution.
Fewer frames only means coarser motion on screen. A walking person
(1.4 m/s) moves about 28 cm, roughly 9 px, between frames at 5 Hz, versus
about 14 cm at 11 Hz. The reliable link (−0.79%) was judged worth more
than the extra updates, which would need the marginal −2.34% link. To
make up for the lower rate, the display interpolates each ping toward its
new position between frames (milestone 4). The 256000 retest is left as
optional extra data for the report.

**Revisited after milestone 3 (2026-11-21).** On hardware, the 5 Hz updates
felt laggy when panning the handheld device. Turning swings the whole scene
past the radar, so 200 ms steps show much more than they do for a walking
person. The radar was set back to its factory rate with
`RADAR_SETUP_BAUD=256000` (index 0x0007). The setup program measured
**5 Hz at 57600, then 11 Hz at 256000**, and all commands were ACKed.

| Radar baud | AVR baud (error) | Frame rate |
|---|---|---|
| 256000 | 250000 (−2.34%) | 11 Hz |
| 57600 | 57143 (−0.79%) | 5 Hz |
| 38400 | 38462 (+0.16%) | 5–6 Hz |

The LD2450 only sends at its full rate at the factory baud. This isn't
documented in the protocol V1.03, which just says "10 frames per second".
The firmware now runs the link at 250000 and relies on the measured
zero-error result plus the parser rejecting bad frames.

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

## Radar view (milestone 3)

- **Geometry:** a 120° fan (±60°, the radar's coverage) with its apex at
  (160, 230) and a 180 px radius for 6 m, i.e. 33.3 mm/px. The fan edge ends
  at 180·sin 60° = 156 px sideways and 180·cos 60° = 90 px up, so the fan
  fills x = 4…316 and y = 50…230.
- **Rings:** every 1.5 m (45 px).
- **Background as a function:** `bg_color_at(x, y)` decides from geometry
  alone whether a pixel is a ring (|d² − r²| ≤ r), a fan edge
  (|dx·90 − dy·156| ≤ 78, one pixel per column) or the center line. The
  same function draws the background once and repairs it wherever a ping,
  label, arrow or the sweep line has been. Repairs can never disagree with
  the original drawing, and no framebuffer is needed.
- **Labels:** drawn "transparent" by streaming bg_color_at() for the
  non-glyph pixels, so the rings stay visible behind text.
- **Sweep:** a line bouncing between ±58° in 2° steps every 40 ms
  (50 °/s). It's erased by redrawing the same Bresenham line with
  per-pixel background colors. A perpendicular-distance test
  (|dx·cos a − dy·sin a|) decides which pings and ring labels it crossed,
  and only those are redrawn.
- **Alternative rejected:** erasing the sweep by drawing it black left gaps
  in every ring it crossed. Redrawing all the rings each step would cost
  ~40 ms.

## Target smoothing (milestone 4)

Observed on hardware with the unfiltered view: blips twitched by ±10–20 cm
on a still target, occasionally jumped or flashed (ghost reflections, slot
hand-overs), and stepped visibly between frames. The fixes, all in
`track.c` / `ui.c`, with constants as `#define`s:

| Stage | Rule | Constant | Why |
|---|---|---|---|
| Range gate | 0.5 m ≤ d ≤ 6 m | `MIN_RANGE_MM`, `MAX_RANGE_MM` | under 0.5 m is the holder (brief); 6 m is the radar's maximum range |
| Confirm | shown after 2 good frames in a row | `CONFIRM_FRAMES` = 2 (~0.18 s) | hides one-frame ghosts |
| Hold | removed after 3 misses in a row | `HOLD_FRAMES` = 3 (~0.27 s) | stops flicker when the radar drops a frame |
| Jump reset | \|raw − smoothed\| > 1.2 m: restart the filter at the new reading | `JUMP_MM` | a different person in the slot shouldn't slide across the screen |
| Adaptive EMA | s += (raw − s)·α, α = 1/4 if \|raw − s\| ≤ 250 mm, else 1/2 | `EMA_SLOW_SHIFT`, `EMA_FAST_SHIFT`, `EMA_FAST_MM` | heavy smoothing when still, quick response when moving or panning |
| Glide | drawn position moves halfway to the goal every 40 ms | `glide()` in ui.c | dots slide between the 90 ms radar frames |

EMA math: alpha = 2^−k, so it's a shift instead of a divide. The state is
kept in mm × 16 (`FP_SHIFT` = 4), so small corrections aren't lost to
truncation. Effect at 11 Hz:
- α = 1/4: time constant ≈ 1/α frames = 4 frames ≈ 0.36 s. Noise std is
  reduced to √(α/(2−α)) = 38% of raw.
- α = 1/2: ≈ 2 frames ≈ 0.18 s, noise reduced to 58% of raw.

A fixed α = 1/4 would lag about 0.36 s whenever the device is panned,
which was the complaint about turning. The adaptive switch keeps the low
noise when still without that lag.

Distance is measured with a Manhattan distance |dx| + |dy| instead of a true
distance. It's cheap (no square root), and for thresholds like these
the ≤ 41% overestimate on diagonals doesn't matter.

**Revision: EMA → alpha-beta filter (2026-11-21).** On hardware, with a
RAW-vs-filtered readout for target 1 on screen, slowly turning the device
showed RAW x moving steadily while the filtered x trailed behind. Any
plain average lags a target that keeps moving in one direction: a slow
turn never produces residuals above 250 mm, so the EMA stayed in its heavy
α = 1/4 mode the whole time. The replacement keeps a velocity estimate
per slot:

```
pred = s + v            r = raw − pred
s    = pred + α·r       v = v + β·r
```

- Gains: α = 1/4, β = 1/16 while |r| ≤ 250 mm; α = 1/2, β = 1/8 above that.
  All are shifts.
- Stability: 0 < α < 1, β > 0, 4 − 2α − β > 0, true for both pairs.
- Velocity is clamped to 300 mm/frame (~3.3 m/s), so one bad reading can't
  fling a target.
- Velocity is zeroed on a missed frame, so a returning target doesn't lurch.

Simulated with the firmware's own integer math (Python, same shifts):

| | Lag on a 30 mm/frame ramp (slow turn) | Std on a still target, raw ±150 mm (std 89) |
|---|---|---|
| Adaptive EMA | 90 mm | 35 mm |
| Alpha-beta | 2 mm | 46 mm |

The trade is slightly more jitter at rest (still half of raw, further
softened by the on-screen glide) for no lag at constant speed.

**Direction arrows:** the last 5 smoothed positions are kept per target.
Movement from the oldest to the newest (4 frames ≈ 0.36 s) is scaled to a
14 px arrow, but only if the target moved ≥ 150 mm (Manhattan) in that
time, about 0.4 m/s. Below that the arrow is hidden, so a standing person
doesn't get a wobbling arrow. The radar's own speed field wasn't used: it
only gives the radial speed (toward or away), not the direction.

## Alert zone hysteresis (milestone 5)

The input is the nearest **confirmed, filtered** target from `track.c`. It's
already range-gated (nothing under 0.5 m) and smoothed, so single-frame
ghosts and jitter can't trigger the alarm on their own.

Two thresholds:
- off → on when nearest < R
- on → off when nearest > R + 250 mm, or when no target is left

With the default R = 1.5 m, the alarm turns on inside 1.50 m and off beyond
1.75 m. The 250 mm band is about 5× the filtered position noise on a still
target (std ≈ 46 mm, simulated), so someone standing at the edge can't
toggle it.

Beep rate is linear in distance:
`period = 100 + (d − 500)·(800 − 100)/(R − 500)` ms, clamped. That gives
800 ms at the zone edge and 100 ms at the 0.5 m minimum range. In the
hysteresis band it beeps at the slowest rate. Mute silences the buzzer only;
the zone ring and ALERT text still work.

Tested on hardware: beeps start crossing 1.5 m, speed up approaching
0.5 m, and stop past ~1.75 m with no chatter at the edge.

**Revised after testing (2026-11-21):** the default radius was raised to
**2.0 m** (exit at 2.25 m), and a **continuous tone below 1.0 m**
(`ALERT_SOLID_MM`) was added at the user's request. The beep period now
runs linearly from 800 ms at R down to 100 ms at 1.0 m:
`period = 100 + (d − 1000)·700/(R − 1000)`. The buzzer's
`BUZZER_SOLID` mode just keeps OC2B connected. Leaving solid mode restarts
the beep cycle, so the change is audible straight away.

## Buzzer tone (Timer2)

A passive buzzer needs an AC drive at audio frequency. Timer2 makes it in
hardware on OC2B (PD3, the fixed pin wired to the buzzer through 220 Ω):

- Mode: CTC, WGM2 = 010, TOP = OCR2A [M328P §18.7.2, Table 18-8].
- Output: COM2B = 01, toggle OC2B on compare match [Table 18-5].
- Prescaler: N = 32, CS2 = 011 [Table 18-9].
- Frequency: f = F_CPU / (2·N·(1 + OCR2A)) = 16 MHz / (2·32·125) =
  **2000 Hz exactly** with OCR2A = OCR2B = 124. OCR2B must be ≤ OCR2A for
  the match to happen.
- Lowest possible tone with /32: OCR2A = 255 gives 977 Hz.

Beeps are made by connecting and disconnecting OC2B (COM2B bits) while the
timer keeps running. Between beeps the pin is driven low, so no DC sits
across the buzzer. Each beep is 40 ms.

Alternatives considered:
- Bit-banging the pin from an ISR at 4 kHz costs CPU and jitters while the
  display is busy.
- Fast PWM mode would also work, but CTC toggle gives an exact 50% duty
  cycle with one register.

## Touch calibration

_TBD_

## EEPROM settings format

_TBD_

## Timer assignments

_TBD_

## Memory budget

_TBD_

## Things that didn't work

### Radar mounted landscape (fixed 2026-11-21)

The LD2450 was first taped to the back plate with its long side
horizontal. Distance (Y) tracked well, but sideways movement didn't. With a
large RAW X readout on screen, walking side to side about 2 m in front showed
RAW X hovering around −300 mm and fluctuating randomly, instead of sweeping
roughly ±1000 mm.

Cause: the module measures azimuth across its **short** side. The manual's
Figure 6 ("Radar wall mounting angle identification", §7, p. 14) shows the
module standing vertically, marked Up/Down, with the positive/negative angle
arrows pointing left/right. Mounted landscape, it was measuring elevation
instead of azimuth.

Fix: the module was rotated 90° on the plate (long side vertical, antenna
still facing out the back). No firmware change; `RADAR_X_SIGN` handles
the left/right sign if the module ends up upside down.

Related notes from the same manual (§7.1, p. 15):
- A metal back plate is recommended to shield the radar's back lobe. The
  aluminum plate already does this.
- "The shaking of the radar itself will affect the detection effect." The
  LD2450 is designed for a fixed wall mount, so handheld use is inherently
  noisier. This is a known limitation of the design and part of the reason
  for the tracking filter.

### Software screen dimming (removed)

To make recording in a dark room easier, the D2 button was set to cycle a
software brightness (100/55/30/15 %): every RGB565 color was scaled per
channel in `ili9341_push_color()`. It worked, but it didn't help. The glare
comes from the **backlight**, which is hard-wired to 5 V through 100 Ω and
lights the whole panel no matter what is drawn, so LCD black still glows.
The feature was removed. Real dimming needs hardware: either a larger
series resistor (permanently dimmer), or a transistor switching the
backlight from a free PWM-capable pin (D5/D6). The debounced button driver
(`button.c`) was kept for later use.

### Testing on dogs

Early tracking tests used the family dogs. They're small, low (near the
bottom of the ±35° elevation cone when the device is held at chest height)
and erratic, and they gave intermittent, jumpy readings. The manufacturer
characterizes the radar with a 1.75 m adult [manual §7, Fig. 7]. Later tests
were done on a person, with the device propped on a chair and the screen
filmed from behind.
