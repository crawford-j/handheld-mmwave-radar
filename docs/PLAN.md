# Firmware Plan

Status: **approved 2026-11-21; milestones 1–5 done.** The sections below are
the original proposal. Where the built design differs, the "Status and scope
changes" section at the end and DESIGN_NOTES.md describe what was actually
done. Datasheet references use the short names in
[datasheets/README.md](datasheets/README.md).

## Module layout

| Module | Responsibility |
|---|---|
| `tick` | Timer0 1 ms system tick, `tick_ms()` |
| `uart_radar` | USART0 init, RX ISR into a ring buffer, frame parser state machine, frame/error counters, config-command sender (setup build only) |
| `spi` | SPI master init, `spi_xfer()`, clock switching between display and touch speed |
| `ili9341` | Reset and init sequence, MADCTL landscape, address window, fill rect, pixel push |
| `gfx` | Lines (Bresenham), circles and arcs (midpoint), 5×7 font in PROGMEM, text and number printing |
| `touch` | XPT2046 X/Y/Z1 reads, averaging, pressure threshold, calibration constants, mapping to screen coordinates |
| `track` | Signed decode, 0.5 m self-filter, distance √(x²+y²), EMA smoothing, motion history for arrows, alert-zone hysteresis |
| `buzzer` | Timer2 tone on OC2B, beep cadence driven from the tick |
| `settings` | EEPROM struct with magic byte, version and checksum; load with defaults fallback; save with `eeprom_update_block` |
| `ui` | Radar view (rings, sweep, pings, labels, arrows), status/debug line, menu screens, partial-redraw bookkeeping |
| `trig` | Sine table in PROGMEM (0–90°) and integer square root |
| `main` | Init order and the main loop |

`printf` is not used: it costs about 1.5 KB of flash and a lot of stack.
Numbers are formatted by a small hand-written routine.

## Peripheral settings

| Peripheral | Setting | Math / reference |
|---|---|---|
| Timer0 | CTC, prescaler 64, OCR0A = 249 → 1 kHz tick | 16e6 / 64 / 250 = 1000 Hz [M328P §15, Table 15-8, 15-9] |
| Timer2 | CTC, toggle OC2B, prescaler 32, OCR2A = OCR2B = 124 → 2 kHz tone | f = 16e6 / (2·32·125) = 2000 Hz [M328P §18, Table 18-5, 18-8, 18-9] |
| Timer1 | Free | — |
| USART0 | 38400 8N1, U2X0 = 0, UBRR0 = 25 (+0.16 %) | [M328P §20.3 Table 20-1, §20.10 Table 20-7] |
| SPI (display) | fosc/4 = 4 MHz, mode 0, MSB first | ILI9341 write cycle ≥ 100 ns (≤ 10 MHz) [ILI9341 serial timing table]; 4 MHz cap set by the level shifter |
| SPI (touch) | fosc/16 = 1 MHz, mode 0 | XPT2046 tCH, tCL ≥ 200 ns (≤ 2.5 MHz) [XPT2046 Table 9]; acquisition is 3 DCLK cycles, so a slower clock gives the panel more settling time [XPT2046 Table 2] |

SPI clock dividers come from [M328P §19, Table 19-5].

The buzzer tone frequency is a `#define`. Passive buzzers are loudest near
their resonant frequency, usually 2–4 kHz.

## Display layout (landscape 320×240)

- **Radar fan:** a 120° fan (±60°, matching the radar's field of view)
  centered at the bottom of the screen. The apex is at (160, 230) with a radius
  of 180 px, set by 180·sin 60° ≈ 156 px fitting in the half-width of 160 px.
  6 m maps to 180 px, about 33 mm/px.
- **Range rings:** 1.5 / 3 / 4.5 / 6 m in meters, or 5 / 10 / 15 / 20 ft in feet.
- **Sweep:** a line that steps across the fan. Each step erases the old line,
  redraws any ring pixels it crossed, and draws the new line.
- **Pings:** a filled 4 px dot per target with a distance label beside it
  (`2.3m` / `7.5ft`). The previous dot, label and arrow are erased by
  redrawing them in the background color.
- **Status line:** one text row at the top (frames, bad frames, overruns),
  shown when debug is on.
- **Menu:** full-screen buttons for units, alert radius −/+, mute and debug,
  plus Back. Leaving the menu saves to EEPROM if anything changed.

There is no framebuffer: a 320×240×16-bit frame would be 150 KB. Every change
is a small address-window write.

## Radar parsing

1. The USART RX ISR pushes each byte into a 64-byte ring buffer. If the buffer
   is full, the byte is dropped and an overrun counter goes up.
2. The main loop drains the buffer into a state machine that looks for the
   `AA FF 03 00` header, collects 24 payload bytes into a scratch buffer, then
   checks for the `55 CC` tail.
3. On a good tail, the scratch buffer is copied into the "latest frame" and a
   flag is set. On a bad tail, the bad-frame counter goes up and the machine
   goes back to header search. A half-written frame is never used.
4. ACK frames (`FD FC FB FA …`) aren't data headers, so the parser skips them.

**One-time baud setup** is a separate build (`make RADAR_SETUP=1`):

1. Open the UART at 250000 with U2X0 = 0 and UBRR0 = 3.
2. Send enable-config, set-baud 0x0003 and restart, checking each ACK.
3. Switch to 38400 and wait for valid data frames.
4. Print each step's result on the screen, retrying up to 3 times.

The error math and risk are in DESIGN_NOTES.md.

## Tracking

- **Decode:** sign-magnitude with an inverted sign bit [LD2450 §2.3 Table 10].
  `X_FLIP` and `Y_FLIP` are `#define`s, plus `SWAP_XY` if the walk test needs it.
- **Self-filter:** drop a target if distance < `MIN_RANGE_MM` (500).
- **Smoothing:** EMA per slot with α = 1/4, done as a shift on a fixed-point
  state. The state resets when a slot goes from empty to occupied, so a new
  target doesn't slide in from its old position.
- **Arrow:** compare the smoothed position now against the one 4 frames
  (0.4 s) ago. Draw a 12 px arrow in that direction if the move is
  ≥ 100 mm; otherwise draw no arrow.
  The radar's speed field is radial only, so it can't give direction.
- **Alert:** look at the nearest valid target.
  - The alert turns on when distance < R and turns off when distance > R + 250 mm.
  - Beep period scales linearly from 800 ms at the zone edge down to 100 ms
    at 0.5 m, with a 40 ms beep each period.
  - Mute stops the tone; the zone still shows.

## Main loop

```
init: tick, spi, ili9341, settings_load, uart_radar, buzzer, touch, sei
loop:
  uart_radar_poll()            drain ring buffer; sets frame_ready
  if frame_ready:              track_update(); ui_draw_targets(); alert_update()
  every 25 ms:                 ui_sweep_step()
  every 30 ms:                 touch_poll() -> ui_menu_or_tap()
  buzzer_service(tick_ms())    start/stop beeps
  if debug and every 250 ms:   ui_status_line()
```

Everything is non-blocking except full-screen fills when entering or leaving
the menu, which take about 0.4 s. Frames lost during that time are counted
and are harmless.

## SRAM budget (2048 B)

| Item | Bytes |
|---|---|
| Ring buffer | 64 |
| Parser scratch + latest frame | 48 |
| Track state (3 × smoothed pos, history, drawn-state for erase) | ~130 |
| UI state (sweep, menu, label buffer) | ~40 |
| Settings, counters, flags | ~30 |
| avr-libc / misc | ~30 |
| **Globals total** | **~350 (target < 500)** |
| Left for stack | **~1.5 KB** |

Fonts, the sine table and all UI strings live in PROGMEM. Expected flash use
is about 8–12 KB of 32 KB.

## EEPROM layout

```
offset 0: magic 0xA5 | 1: version | 2: units | 3: alert radius (×0.1 m)
       4: flags (mute, debug) | 5: checksum (8-bit sum of bytes 0..4, inverted)
```

A wrong magic, version or checksum, or an out-of-range value, loads defaults:
meters, 1.5 m radius, unmuted, debug off.

## Milestones

1. Display init + test pattern
2. Raw radar numbers + counters
3. Radar view: rings, sweep, pings
4. Smoothing + arrows
5. Alert zone + buzzer
6. Touch raw → calibration → menu
7. EEPROM settings

## Status and scope changes (2026-11-22)

| Milestone | Status |
|---|---|
| 1. Display init + test pattern | done |
| 2. Raw radar numbers + counters | done |
| 3. Radar view | done, then redesigned as a dome with a pulse scan |
| 4. Smoothing + arrows | done (alpha-beta filter, glide, confirm/hold) |
| 5. Alert zone + buzzer | done (1.5 m, solid tone < 1 m, arming, re-arm button) |
| 6. Touch: raw → calibration → menu | next |
| 7. EEPROM settings | after 6 |

Differences from the original proposal:
- **Radar link:** runs at 250000 baud (radar at its factory 256000), not
  38400. The radar only sends its full ~11 frames/s at 256000, and the
  250000 link measured zero errors. See DESIGN_NOTES "Radar UART baud rate".
- **Display:** a half-dome with the ±60° coverage cone and an expanding
  pulse. The rotating sweep line is kept as a build option
  (`UI_SCAN_PULSE=0`) and will be a menu choice.
- **Handheld mode:** tried and removed. The radar can't separate its own
  motion from targets without a gyro. The device is used set down or
  mounted.
- **D2 button:** re-arms the alarm (quiet countdown) instead of being
  optional.

### Milestone 6 scope (touch, stylus-friendly)

1. Raw XPT2046 readout (X, Y, Z1 pressure) and corner calibration constants.
2. Tap anywhere to open the menu:
   - Units: m / ft (labels, rings, zone text)
   - Alert radius: − / + in 0.5 m steps (1.0–4.0 m)
   - Mute on/off
   - Debug readout on/off
   - Scan style: pulse / sweep line
   - Theme: 3 presets, or custom with primary / secondary / tertiary colors
     picked from an on-screen palette
3. Colors become runtime variables (a theme struct) instead of `#define`s.
   The dome gradient is derived from the primary color.

### Milestone 7 scope (EEPROM)

All menu settings, including the theme colors, are saved with a magic
byte, version and checksum. Defaults are used on a blank or corrupt EEPROM,
and writes use `eeprom_update_*` so unchanged bytes aren't rewritten.
