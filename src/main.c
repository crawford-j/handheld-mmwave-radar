/*
 * main.c - Handheld mmWave radar firmware entry point.
 *
 * Target: ATmega328P @ 16 MHz (Arduino Uno R3), avr-gcc, no Arduino core.
 *
 * Milestone 1: display bring-up. Shows full-screen color fills, then a static
 * test pattern with text, color bars, a circle, lines, the full character
 * set, and the measured full-screen fill time.
 */

#include "board.h"
#include <avr/pgmspace.h>
#include <util/delay.h>
#include "spi.h"
#include "ili9341.h"
#include "gfx.h"

/*
 * fill_with_label - fill the screen with `bg` and print `name` in the middle,
 * so color order (RGB vs BGR) can be checked by eye.
 */
static void fill_with_label(uint16_t bg, uint16_t fg, const char *name)
{
    gfx_fill_screen(bg);
    gfx_text_P(112, 108, name, fg, bg, 3);
    _delay_ms(700);
}

/*
 * measure_fill_ms - time one full-screen fill with Timer1.
 *
 * Timer1 runs freely here as a stopwatch (it is not used elsewhere yet):
 *   TCCR1A = 0          normal mode, OC1A/OC1B disconnected [M328P §16.11.1]
 *   CS12:0 = 101        clk/1024 [M328P Table 16-5]
 *   tick = 1024 / 16 MHz = 64 us; 16-bit counter wraps after 4.19 s.
 * Returns milliseconds = ticks * 64 / 1000.
 */
static uint16_t measure_fill_ms(uint16_t color)
{
    TCCR1A = 0;
    TCCR1B = (1 << CS12) | (1 << CS10);
    TCNT1  = 0;

    gfx_fill_screen(color);

    uint16_t ticks = TCNT1;
    TCCR1B = 0;                         /* stop the timer */
    return (uint16_t)(((uint32_t)ticks * 64) / 1000);
}

/*
 * draw_test_pattern - the static milestone 1 screen.
 */
static void draw_test_pattern(uint16_t fill_ms)
{
    /* Border on the outermost pixels: all four edges must be visible,
     * which confirms the 320x240 window and landscape orientation. */
    gfx_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COLOR_WHITE);

    /* Origin marker: the red square must sit in the top-left corner. */
    ili9341_fill_rect(2, 2, 8, 8, COLOR_RED);
    gfx_text_P(12, 3, PSTR("(0,0)"), COLOR_WHITE, COLOR_BLACK, 1);

    /* Title, scale 2 (12x16 px per character). */
    gfx_text_P(64, 16, PSTR("M1 DISPLAY TEST"), COLOR_YELLOW, COLOR_BLACK, 2);

    /* Color bars with labels underneath. */
    ili9341_fill_rect(16, 44, 56, 32, COLOR_RED);
    ili9341_fill_rect(80, 44, 56, 32, COLOR_GREEN);
    ili9341_fill_rect(144, 44, 56, 32, COLOR_BLUE);
    gfx_text_P(35, 80, PSTR("RED"), COLOR_WHITE, COLOR_BLACK, 1);
    gfx_text_P(93, 80, PSTR("GREEN"), COLOR_WHITE, COLOR_BLACK, 1);
    gfx_text_P(160, 80, PSTR("BLUE"), COLOR_WHITE, COLOR_BLACK, 1);

    /* Circle test: outline, filled center, and two diagonals through it. */
    gfx_circle(260, 90, 44, COLOR_WHITE);
    gfx_fill_circle(260, 90, 14, COLOR_CYAN);
    gfx_line(216, 46, 304, 134, COLOR_MAGENTA);
    gfx_line(216, 134, 304, 46, COLOR_MAGENTA);

    /* Concentric arcs, like the future range rings. */
    gfx_circle(108, 148, 16, COLOR_DKGREEN);
    gfx_circle(108, 148, 32, COLOR_DKGREEN);
    gfx_circle(108, 148, 48, COLOR_GREEN);

    /* Every printable character, two rows of 48 at scale 1. */
    int16_t x = 4;
    int16_t y = 200;
    for (char c = 0x20; c <= 0x7E; c++) {
        if (c == 0x20 + 48) {
            x = 4;
            y += GFX_CHAR_H + 2;
        }
        x = gfx_char(x, y, c, COLOR_WHITE, COLOR_BLACK, 1);
    }

    /* Measured fill time, for the design notes. */
    x = gfx_text_P(184, 150, PSTR("FILL: "), COLOR_GREEN, COLOR_BLACK, 1);
    x = gfx_uint(x, 150, fill_ms, COLOR_GREEN, COLOR_BLACK, 1);
    gfx_text_P(x, 150, PSTR(" ms"), COLOR_GREEN, COLOR_BLACK, 1);
}

/*
 * main - firmware entry point.
 */
int main(void)
{
    spi_init();
    ili9341_init();

    fill_with_label(COLOR_RED,   COLOR_WHITE, PSTR("RED"));
    fill_with_label(COLOR_GREEN, COLOR_BLACK, PSTR("GREEN"));
    fill_with_label(COLOR_BLUE,  COLOR_WHITE, PSTR("BLUE"));
    fill_with_label(COLOR_WHITE, COLOR_BLACK, PSTR("WHITE"));

    uint16_t fill_ms = measure_fill_ms(COLOR_BLACK);
    draw_test_pattern(fill_ms);

    for (;;) {
    }
}
