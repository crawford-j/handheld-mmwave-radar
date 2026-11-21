/*
 * ili9341.h - ILI9341 TFT driver, 4-line SPI, landscape 320x240, RGB565.
 */

#ifndef ILI9341_H
#define ILI9341_H

#include <stdint.h>

#define LCD_WIDTH   320
#define LCD_HEIGHT  240

/* RGB565: 5 bits red, 6 bits green, 5 bits blue [ILI9341 §7.6.2, 16 bpp]. */
#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     RGB565(255, 0, 0)
#define COLOR_GREEN   RGB565(0, 255, 0)
#define COLOR_BLUE    RGB565(0, 0, 255)
#define COLOR_YELLOW  RGB565(255, 255, 0)
#define COLOR_CYAN    RGB565(0, 255, 255)
#define COLOR_MAGENTA RGB565(255, 0, 255)
#define COLOR_GRAY    RGB565(128, 128, 128)
#define COLOR_DKGREEN RGB565(0, 96, 0)

void ili9341_init(void);
void ili9341_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void ili9341_push_color(uint16_t color, uint32_t count);
void ili9341_end(void);
void ili9341_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void ili9341_draw_pixel(int16_t x, int16_t y, uint16_t color);
void ili9341_set_brightness(uint8_t level);

#endif /* ILI9341_H */
