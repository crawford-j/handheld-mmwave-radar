/*
 * ili9341.c - ILI9341 TFT driver written from the ILITEK datasheet V1.02.
 *
 * Interface: 4-line serial (SCL, SDA, CSX, D/CX) [ILI9341 §7.6.2].
 *   D/CX low  -> the byte is a command
 *   D/CX high -> the byte is a parameter or pixel data
 * The display is write-only here (SDO is not connected).
 *
 * Only the commands this project needs are used; every other register keeps
 * the power-on default loaded from NV memory during reset [ILI9341 §15.4].
 */

#include "board.h"
#include <util/delay.h>
#include "spi.h"
#include "ili9341.h"

/* ---- Command bytes [ILI9341 §8.1 command list] ---- */
#define CMD_SWRESET  0x01  /* Software Reset          [§8.2.2]  */
#define CMD_SLPOUT   0x11  /* Sleep Out               [§8.2.12] */
#define CMD_DISPON   0x29  /* Display ON              [§8.2.19] */
#define CMD_CASET    0x2A  /* Column Address Set      [§8.2.20] */
#define CMD_PASET    0x2B  /* Page Address Set        [§8.2.21] */
#define CMD_RAMWR    0x2C  /* Memory Write            [§8.2.22] */
#define CMD_MADCTL   0x36  /* Memory Access Control   [§8.2.29] */
#define CMD_COLMOD   0x3A  /* Pixel Format Set        [§8.2.33] */

/* ---- MADCTL bits [ILI9341 §8.2.29] ---- */
#define MADCTL_MY    0x80  /* row address order (flip vertical)      */
#define MADCTL_MX    0x40  /* column address order (flip horizontal) */
#define MADCTL_MV    0x20  /* row/column exchange (landscape)        */
#define MADCTL_ML    0x10  /* vertical refresh order                 */
#define MADCTL_BGR   0x08  /* 1 = panel has a BGR color filter       */
#define MADCTL_MH    0x04  /* horizontal refresh order               */

/*
 * Landscape orientation. MV swaps rows and columns, so the 240x320 native
 * panel is addressed as 320 wide x 240 tall. MX/MY pick which corner is
 * (0,0); adjust after the milestone 1 test if the image is mirrored.
 * BGR = 1 because these panels use a BGR filter; if red and blue appear
 * swapped on the test pattern, clear it.
 */
#ifndef LCD_MADCTL
#define LCD_MADCTL   (MADCTL_MV | MADCTL_BGR)
#endif

/* ---- Pin helpers ---- */
#define CS_LOW()     (LCD_PORT &= ~(1 << LCD_CS))
#define CS_HIGH()    (LCD_PORT |=  (1 << LCD_CS))
#define DC_CMD()     (LCD_PORT &= ~(1 << LCD_DC))
#define DC_DATA()    (LCD_PORT |=  (1 << LCD_DC))

/*
 * write_cmd - send one command byte (D/CX low). CS must already be low.
 */
static void write_cmd(uint8_t cmd)
{
    DC_CMD();
    spi_xfer(cmd);
    DC_DATA();          /* following bytes are parameters/data */
}

/*
 * write_cmd_1 - send a command with a single parameter byte, as its own
 * CS-framed transaction.
 */
static void write_cmd_1(uint8_t cmd, uint8_t param)
{
    CS_LOW();
    write_cmd(cmd);
    spi_xfer(param);
    CS_HIGH();
}

/*
 * write_cmd_0 - send a command with no parameters.
 */
static void write_cmd_0(uint8_t cmd)
{
    CS_LOW();
    write_cmd(cmd);
    CS_HIGH();
}

/*
 * ili9341_init - hardware reset and minimal init sequence.
 * spi_init() must be called first.
 */
void ili9341_init(void)
{
    /* RST and DC as outputs (CS was set up by spi_init). */
    LCD_DDR  |= (1 << LCD_RST) | (1 << LCD_DC);
    LCD_PORT |= (1 << LCD_DC);
    spi_speed_display();

    /* Hardware reset: RESX low >= 10 us (tRW), then wait the reset-cancel
     * time tRT, up to 120 ms, before sending commands [ILI9341 §15.4]. */
    LCD_PORT |= (1 << LCD_RST);
    _delay_ms(5);
    LCD_PORT &= ~(1 << LCD_RST);
    _delay_us(20);                      /* > tRW = 10 us */
    LCD_PORT |= (1 << LCD_RST);
    _delay_ms(120);                     /* tRT max = 120 ms */

    /* Software reset as well, so a warm restart (AVR reset without power
     * cycle) starts from the same state. Wait 5 ms before the next command;
     * 120 ms if the panel was in Sleep Out [ILI9341 §8.2.2]. */
    write_cmd_0(CMD_SWRESET);
    _delay_ms(120);

    /* Leave sleep mode: wait 5 ms before the next command and 120 ms before
     * Sleep In; the booster and oscillator start here [ILI9341 §8.2.12]. */
    write_cmd_0(CMD_SLPOUT);
    _delay_ms(120);

    /* Pixel format: parameter 0x55 = DPI 101 (16 bpp RGB interface, unused)
     * and DBI 101 (16 bpp MCU interface). Default 0x66 is 18 bpp
     * [ILI9341 §8.2.33]. 16 bpp = 2 bytes/pixel instead of 3. */
    write_cmd_1(CMD_COLMOD, 0x55);

    /* Scan direction / color order, see LCD_MADCTL above [ILI9341 §8.2.29]. */
    write_cmd_1(CMD_MADCTL, LCD_MADCTL);

    /* Turn the display output on [ILI9341 §8.2.19]. */
    write_cmd_0(CMD_DISPON);
    _delay_ms(20);
}

/*
 * ili9341_set_window - select the rectangle (x0,y0)-(x1,y1), inclusive, and
 * start a Memory Write. Leaves CS low with D/CX high so pixel data can follow
 * with ili9341_push_color(); finish with ili9341_end().
 *
 * CASET takes SC[15:0], EC[15:0] (high byte first) [ILI9341 §8.2.20];
 * PASET takes SP[15:0], EP[15:0] [ILI9341 §8.2.21]. With MV = 1, columns run
 * 0..319 and pages 0..239.
 */
void ili9341_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    CS_LOW();
    write_cmd(CMD_CASET);
    spi_xfer(x0 >> 8);
    spi_xfer(x0 & 0xFF);
    spi_xfer(x1 >> 8);
    spi_xfer(x1 & 0xFF);

    write_cmd(CMD_PASET);
    spi_xfer(y0 >> 8);
    spi_xfer(y0 & 0xFF);
    spi_xfer(y1 >> 8);
    spi_xfer(y1 & 0xFF);

    /* Memory Write: following data fills the window left-to-right,
     * top-to-bottom [ILI9341 §8.2.22]. */
    write_cmd(CMD_RAMWR);
}

/*
 * ili9341_push_color - send the same RGB565 color `count` times into the
 * window opened by ili9341_set_window(). High byte first [ILI9341 §7.6.2].
 */
void ili9341_push_color(uint16_t color, uint32_t count)
{
    uint8_t hi = color >> 8;
    uint8_t lo = color & 0xFF;

    while (count--) {
        spi_xfer(hi);
        spi_xfer(lo);
    }
}

/*
 * ili9341_end - end the current Memory Write by releasing CS.
 */
void ili9341_end(void)
{
    CS_HIGH();
}

/*
 * ili9341_fill_rect - fill a w x h rectangle at (x,y), clipped to the screen.
 */
void ili9341_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color)
{
    /* Clip to the visible area. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH)  w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) {
        return;
    }

    ili9341_set_window(x, y, x + w - 1, y + h - 1);
    ili9341_push_color(color, (uint32_t)w * (uint32_t)h);
    ili9341_end();
}

/*
 * ili9341_draw_pixel - set one pixel, ignoring points off the screen.
 */
void ili9341_draw_pixel(int16_t x, int16_t y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT) {
        return;
    }
    ili9341_set_window(x, y, x, y);
    ili9341_push_color(color, 1);
    ili9341_end();
}
