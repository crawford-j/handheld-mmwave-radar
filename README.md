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
- Arming delay at power-up, and a button to re-arm after mounting
- Touch menu (in progress): units (m / ft), alert radius, mute, scan style,
  and color themes (3 presets plus custom primary/secondary/tertiary colors)
- Settings saved to EEPROM with a checksum (in progress)

## Hardware

| Part | Role |
|---|---|
| Arduino Uno R3 (ATmega328P, 16 MHz) | Main controller |
| HLK-LD2450 | 24 GHz mmWave radar, UART |
| ILI9341 2.8" TFT + XPT2046 | Display and resistive touch, SPI |
| TXS0108E | 5 V / 3.3 V level shifter |
| Passive buzzer | Alert tone (Timer2 / OC2B) |
| Push button | Re-arm the alarm |

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
```

The radar's TX wire must be disconnected from D0 while uploading.

## Repository layout

```
src/              firmware sources
tools/            font and sine table generators (Python)
docs/datasheets/  component datasheet sources
docs/PLAN.md      module plan and milestones
DESIGN_NOTES.md   design decisions, calculations, measurements and alternatives
Makefile          avr-gcc build
```
