/*
 * board.h - Pin map and clock for the handheld radar (Arduino Uno R3).
 *
 * Wiring is fixed (see README.md). 3.3 V devices sit behind a TXS0108E level
 * shifter, except MISO (touch T_DO) and radar TX, which go straight to the AVR.
 */

#ifndef BOARD_H
#define BOARD_H

/* CPU clock. Normally passed by the Makefile / Microchip Studio symbols;
 * this fallback keeps util/delay.h correct if it is not. */
#ifndef F_CPU
#define F_CPU 16000000UL
#endif

#include <avr/io.h>

/* ---- SPI bus (hardware SPI pins, fixed by the AVR) [M328P §19.2] ---- */
#define SPI_DDR        DDRB
#define SPI_MOSI       PB3     /* D11 -> display SDI + touch T_DIN (shifter B4/A4) */
#define SPI_MISO       PB4     /* D12 <- touch T_DO (direct)                       */
#define SPI_SCK        PB5     /* D13 -> display SCK + touch T_CLK (shifter B5/A5) */

/* ---- ILI9341 display control ---- */
#define LCD_PORT       PORTB
#define LCD_DDR        DDRB
#define LCD_CS         PB2     /* D10, also the SPI SS pin (shifter B1/A1) */
#define LCD_DC         PB1     /* D9,  low = command, high = data (shifter B3/A3) */
#define LCD_RST        PB0     /* D8,  active low reset (shifter B2/A2)    */

/* ---- XPT2046 touch ---- */
#define TOUCH_PORT     PORTD
#define TOUCH_DDR      DDRD
#define TOUCH_CS       PD7     /* D7 (shifter B6/A6) */

#endif /* BOARD_H */
