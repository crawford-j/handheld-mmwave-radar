/*
 * gfx.c - Lines, circles and text drawn straight to the display.
 *
 * There is no framebuffer (320x240x2 B = 150 KB vs 2 KB of SRAM), so every
 * primitive writes pixels or small rectangles to the ILI9341 directly.
 * Erasing means drawing the same shape again in the background color.
 */

#include <avr/pgmspace.h>
#include "ili9341.h"
#include "font5x7.h"
#include "gfx.h"

/*
 * gfx_fill_screen - fill the whole display with one color.
 * 320*240 = 76800 pixels * 2 bytes = 153600 bytes over SPI.
 */
void gfx_fill_screen(uint16_t color)
{
    ili9341_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, color);
}

/*
 * gfx_hline - horizontal line of width w starting at (x,y).
 * A 1-pixel-tall rectangle: one window write instead of w pixel writes.
 */
void gfx_hline(int16_t x, int16_t y, int16_t w, uint16_t color)
{
    ili9341_fill_rect(x, y, w, 1, color);
}

/*
 * gfx_vline - vertical line of height h starting at (x,y).
 */
void gfx_vline(int16_t x, int16_t y, int16_t h, uint16_t color)
{
    ili9341_fill_rect(x, y, 1, h, color);
}

/*
 * gfx_rect - 1-pixel rectangle outline.
 */
void gfx_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    gfx_hline(x, y, w, color);
    gfx_hline(x, y + h - 1, w, color);
    gfx_vline(x, y, h, color);
    gfx_vline(x + w - 1, y, h, color);
}

/*
 * gfx_line - line from (x0,y0) to (x1,y1) using Bresenham's algorithm.
 *
 * Integer only: err tracks the distance between the true line and the pixel
 * grid. Each step moves in x, y, or both depending on the sign of 2*err.
 * Works in all octants because sx/sy carry the direction and dy is negated.
 */
void gfx_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    int16_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int16_t dy = (y1 > y0) ? (y0 - y1) : (y1 - y0);   /* -|dy| */
    int8_t  sx = (x0 < x1) ? 1 : -1;
    int8_t  sy = (y0 < y1) ? 1 : -1;
    int16_t err = dx + dy;

    for (;;) {
        ili9341_draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int16_t e2 = 2 * err;
        if (e2 >= dy) {         /* step in x */
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {         /* step in y */
            err += dx;
            y0 += sy;
        }
    }
}

/*
 * gfx_circle - circle outline of radius r centered at (cx,cy), using the
 * midpoint circle algorithm.
 *
 * Only one octant (from 0 to 45 degrees) is computed; the other seven come
 * from symmetry. err is the midpoint decision value: when it goes positive,
 * the next pixel steps inward in x.
 */
void gfx_circle(int16_t cx, int16_t cy, int16_t r, uint16_t color)
{
    int16_t x = r;
    int16_t y = 0;
    int16_t err = 1 - r;

    while (x >= y) {
        ili9341_draw_pixel(cx + x, cy + y, color);
        ili9341_draw_pixel(cx + y, cy + x, color);
        ili9341_draw_pixel(cx - y, cy + x, color);
        ili9341_draw_pixel(cx - x, cy + y, color);
        ili9341_draw_pixel(cx - x, cy - y, color);
        ili9341_draw_pixel(cx - y, cy - x, color);
        ili9341_draw_pixel(cx + y, cy - x, color);
        ili9341_draw_pixel(cx + x, cy - y, color);

        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

/*
 * gfx_fill_circle - filled circle, drawn as horizontal spans from the same
 * midpoint algorithm (4 spans per step instead of 8 pixels).
 */
void gfx_fill_circle(int16_t cx, int16_t cy, int16_t r, uint16_t color)
{
    int16_t x = r;
    int16_t y = 0;
    int16_t err = 1 - r;

    while (x >= y) {
        gfx_hline(cx - x, cy + y, 2 * x + 1, color);
        gfx_hline(cx - x, cy - y, 2 * x + 1, color);
        gfx_hline(cx - y, cy + x, 2 * y + 1, color);
        gfx_hline(cx - y, cy - x, 2 * y + 1, color);

        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

/*
 * gfx_char - draw one character with its top-left at (x,y).
 *
 * Opaque: the whole 6x8 cell (times scale) is written, background included,
 * so redrawing text over old text erases it without a separate clear.
 * The cell is one address window filled in raster order, which is far
 * faster than drawing pixels one by one.
 * Returns the x position for the next character.
 */
int16_t gfx_char(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale)
{
    int16_t w = GFX_CHAR_W * scale;
    int16_t h = GFX_CHAR_H * scale;

    if (c < FONT_FIRST || c > FONT_LAST) {
        c = '?';
    }
    if (x < 0 || y < 0 || x + w > LCD_WIDTH || y + h > LCD_HEIGHT) {
        return x + w;           /* skip cells that would not fit */
    }

    const uint8_t *glyph = font5x7[c - FONT_FIRST];

    ili9341_set_window(x, y, x + w - 1, y + h - 1);
    for (uint8_t row = 0; row < GFX_CHAR_H; row++) {
        for (uint8_t sy = 0; sy < scale; sy++) {
            for (uint8_t col = 0; col < GFX_CHAR_W; col++) {
                /* Column 5 and row 7 are the spacing gap: always bg. */
                uint8_t bits = (col < FONT_W) ? pgm_read_byte(&glyph[col]) : 0;
                uint16_t color = (bits & (1 << row)) ? fg : bg;
                ili9341_push_color(color, scale);
            }
        }
    }
    ili9341_end();
    return x + w;
}

/*
 * gfx_text - draw a string from RAM. Returns the x after the last character.
 */
int16_t gfx_text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale)
{
    while (*s) {
        x = gfx_char(x, y, *s++, fg, bg, scale);
    }
    return x;
}

/*
 * gfx_text_P - draw a string stored in flash (PSTR / PROGMEM), so constant
 * UI text does not use SRAM.
 */
int16_t gfx_text_P(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale)
{
    char c;
    while ((c = pgm_read_byte(s++)) != '\0') {
        x = gfx_char(x, y, c, fg, bg, scale);
    }
    return x;
}

/*
 * gfx_uint - draw an unsigned number in decimal, without printf.
 * Digits are produced least significant first into a small buffer, then
 * drawn in reverse.
 */
int16_t gfx_uint(int16_t x, int16_t y, uint16_t n, uint16_t fg, uint16_t bg, uint8_t scale)
{
    char buf[5];                /* 65535 has 5 digits */
    uint8_t i = 0;

    do {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    } while (n);

    while (i) {
        x = gfx_char(x, y, buf[--i], fg, bg, scale);
    }
    return x;
}

/*
 * gfx_int_pad - draw a signed number right-aligned in a field of `width`
 * characters, padded with spaces on the left.
 *
 * Because text is opaque and the field width is fixed, redrawing a value in
 * the same place fully erases the old one, even when it had more digits.
 * If the number needs more than `width` characters, it is drawn in full.
 */
int16_t gfx_int_pad(int16_t x, int16_t y, int32_t n, uint8_t width,
                    uint16_t fg, uint16_t bg, uint8_t scale)
{
    char buf[12];                       /* "-2147483648" + spare */
    uint8_t i = 0;
    uint8_t neg = (n < 0);
    uint32_t u = neg ? (uint32_t)(-n) : (uint32_t)n;

    do {                                /* digits, least significant first */
        buf[i++] = '0' + (u % 10);
        u /= 10;
    } while (u);
    if (neg) {
        buf[i++] = '-';
    }

    for (uint8_t pad = i; pad < width; pad++) {
        x = gfx_char(x, y, ' ', fg, bg, scale);
    }
    while (i) {
        x = gfx_char(x, y, buf[--i], fg, bg, scale);
    }
    return x;
}

/*
 * gfx_hex8 - draw one byte as two uppercase hex digits.
 */
int16_t gfx_hex8(int16_t x, int16_t y, uint8_t v, uint16_t fg, uint16_t bg, uint8_t scale)
{
    static const char hex[] PROGMEM = "0123456789ABCDEF";
    x = gfx_char(x, y, pgm_read_byte(&hex[v >> 4]), fg, bg, scale);
    return gfx_char(x, y, pgm_read_byte(&hex[v & 0x0F]), fg, bg, scale);
}
