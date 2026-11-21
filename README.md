# Handheld mmWave Radar

Final project for ECE:3360 Embedded Systems, University of Iowa, Fall 2026.

A handheld presence radar. An HLK-LD2450 24 GHz radar module tracks up to three
people, an ATmega328P reads their positions over UART, and a 2.8" ILI9341 TFT
with an XPT2046 touch controller shows them on a radar-style display.

## Features

- Radar view with range rings, a rotating sweep line and a labeled ping per target
- Smoothed target tracking with direction arrows
- Alert zone with a passive buzzer that beeps faster as a target gets closer
- Touch menu for units (m / ft), alert radius and mute
- Settings saved to EEPROM and restored at power-up

## Hardware

| Part | Role |
|---|---|
| Arduino Uno R3 (ATmega328P, 16 MHz) | Main controller |
| HLK-LD2450 | 24 GHz mmWave radar, UART |
| ILI9341 2.8" TFT + XPT2046 | Display and resistive touch, SPI |
| TXS0108E | 5 V / 3.3 V level shifter |
| Passive buzzer | Alert tone (Timer2 / OC2B) |

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
| Optional button | D2 | PD2 / INT0 |

## Building

Firmware is plain C on avr-libc, with no Arduino core or libraries.

```sh
make        # build and print flash/SRAM usage
make flash  # upload with avrdude (set PORT in the Makefile)
```

The radar's TX wire must be disconnected from D0 while uploading.

## Repository layout

```
src/              firmware sources
docs/datasheets/  component datasheets
DESIGN_NOTES.md   design decisions, calculations and alternatives
Makefile          avr-gcc build
```
