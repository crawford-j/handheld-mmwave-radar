# mmWave Presence Radar

Final project for ECE:3360 Embedded Systems, University of Iowa, Fall 2026.

A portable, battery-powered presence radar: set it down or mount it facing a
room, and it shows where people are and sounds an alarm when someone comes
too close. An HLK-LD2450 24 GHz radar module tracks up to three people within
a 120° cone out to 6 m. An ATmega328P reads their positions over UART, filters
them, and draws a radar display on a 2.8" ILI9341 TFT with an XPT2046 touch
controller.

## Features

- Radar view: a half-dome with range rings, an expanding pulse (or a rotating
  sweep line), and a ping per target labeled with its distance
- Target tracking: alpha-beta filter with velocity prediction, confirm/hold
  ghost rejection, gliding pings and direction arrows
- Alert zone (default 1.5 m) with hysteresis: a passive buzzer beeps faster as
  a target gets closer, and holds a solid tone under 1 m
- Boots into standby: place the device, press the button (or tap the
  screen) to start an 8 s arming countdown; no pings are shown while arming
- Standby again at any time: hold the button 1.5 s to blank the screen and
  disable the alarm. A short press while running re-arms
- Tap the speaker icon on the radar view to mute or unmute (saved)
- Looping animation on the standby screen, converted from any GIF
- Touch menu (stylus): units (m / ft), alert radius, minimum range
  (0.5-1.5 m, the area inside is blacked out on the view), debug overlay,
  scan style (pulse or sweep line), and color themes: 3 presets, or custom
  primary/secondary/tertiary colors from a palette
- Settings saved to EEPROM (magic byte, version, checksum) and restored at
  power-up; reset to defaults from the menu

## Hardware

| Part | Role |
|---|---|
| Arduino Uno R3 (ATmega328P, 16 MHz) | Main controller |
| HLK-LD2450 | 24 GHz mmWave radar, UART |
| ILI9341 2.8" TFT + XPT2046 | Display and resistive touch, SPI |
| TXS0108E | 5 V / 3.3 V level shifter |
| Passive buzzer | Alert tone (Timer2 / OC2B) |
| Push button | Re-arm the alarm (short press), standby (hold 1.5 s) |

### Pin map

| Signal | Uno pin | AVR pin |
|---|---|---|
| Display CS | D10 | PB2 |
| Display DC | D9 | PB1 |
| Display RESET | D8 | PB0 |
| MOSI (display + touch) | D11 | PB3 |
| MISO (touch) | D12 | PB4 |
| SCK (display + touch) | D13 | PB5 |
| Touch CS | D7 | PD7 |
| Radar TX -> AVR RX | D0 | PD0 |
| AVR TX -> radar RX | D1 | PD1 |
| Buzzer | D3 | PD3 / OC2B |
| Button (to GND) | D2 | PD2 |

## Building

Firmware is plain C on avr-libc, with no Arduino core or libraries.

```sh
make                 # build and print flash/SRAM usage
make flash           # upload with avrdude (set PORT in the Makefile)
make DEMO=1          # synthetic targets instead of the radar, for UI testing
make RADAR_SETUP=1   # one-time radar baud configuration tool
make TOUCH_CAL=1     # touch-screen calibration tool
```

To change the standby animation (needs Python with Pillow), then rebuild:

```sh
python tools/gen_anim.py my.gif            # light subject on dark background
python tools/gen_anim.py my.gif --invert   # dark subject on light background
python tools/gen_anim.py                   # back to the built-in radar
```

GIFs that can't be published (e.g. broadcast clips) go through `--local`:
the frames are written to `src/local/`, which is git-ignored and used by
the build instead of the built-in animation. Delete `src/local/` to go back.
Run `make clean` after adding or removing it.

```sh
python tools/gen_anim.py stare.gif --pick 26,29,32,35,38,41,44,47,50 --size 80 --crop 120,0,560,362 --threshold 28 --local
```

The radar's TX wire must be disconnected from D0 while uploading.

## Repository layout

```
src/              firmware sources
tools/            font, sine table and standby animation generators (Python)
docs/datasheets/  component datasheet sources
docs/PLAN.md      module plan and milestones
DESIGN_NOTES.md   design decisions, calculations, measurements and alternatives
Makefile          avr-gcc build
```
