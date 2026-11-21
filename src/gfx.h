/*
 * gfx.h - Drawing primitives on top of the ILI9341 driver (no framebuffer).
 */

#ifndef GFX_H
#define GFX_H

#include <stdint.h>

#define GFX_CHAR_W  6   /* 5 px glyph + 1 px spacing, at scale 1 */
#define GFX_CHAR_H  8   /* 7 px glyph + 1 px spacing, at scale 1 */

void gfx_fill_screen(uint16_t color);
void gfx_hline(int16_t x, int16_t y, int16_t w, uint16_t color);
void gfx_vline(int16_t x, int16_t y, int16_t h, uint16_t color);
void gfx_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void gfx_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
void gfx_circle(int16_t cx, int16_t cy, int16_t r, uint16_t color);
void gfx_fill_circle(int16_t cx, int16_t cy, int16_t r, uint16_t color);

int16_t gfx_char(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale);
int16_t gfx_text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);
int16_t gfx_text_P(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg, uint8_t scale);
int16_t gfx_uint(int16_t x, int16_t y, uint16_t n, uint16_t fg, uint16_t bg, uint8_t scale);
int16_t gfx_int_pad(int16_t x, int16_t y, int32_t n, uint8_t width,
                    uint16_t fg, uint16_t bg, uint8_t scale);
int16_t gfx_hex8(int16_t x, int16_t y, uint8_t v, uint16_t fg, uint16_t bg, uint8_t scale);

#endif /* GFX_H */
