/*
 * touch.c - XPT2046 resistive touch controller, written from the XPTEK
 * datasheet.
 *
 * Bus: shares SPI with the display. Touch CS is PD7; the display's CS must
 * be high during a touch read (it always is between display transactions).
 * The XPT2046 needs a slower clock than the display, so SCK is switched to
 * fosc/16 = 1 MHz for each read and back to 4 MHz afterwards (spi.c).
 *
 * Control byte [XPT2046 Table 6, 7]:
 *   bit 7    S        = 1   start bit
 *   bit 6-4  A2..A0         channel
 *   bit 3    MODE     = 0   12-bit conversion
 *   bit 2    SER/DFR  = 0   differential (ratiometric) reference, which the
 *                           datasheet recommends for X, Y and pressure:
 *                           the reference is the voltage across the panel,
 *                           so supply drift cancels out
 *   bit 1-0  PD1..PD0 = 00  power down between conversions, PENIRQ enabled
 *                           [Table 8]
 * Channels [Table 4/5, differential mode]:
 *   X position A = 101 -> 0xD0     Y position A = 001 -> 0x90
 *   Z1         A = 011 -> 0xB0     Z2         A = 100 -> 0xC0
 *
 * One conversion = 24 clocks [Figure 12]: 8 clocks to send the control
 * byte, 1 clock BUSY, then 12 result bits MSB first, then zeros. Reading
 * the two bytes after the control byte gives
 *   [0 b11 b10 b9 b8 b7 b6 b5] [b4 b3 b2 b1 b0 0 0 0]
 * so the result is (hi << 8 | lo) >> 3.
 *
 * Noise: a resistive panel needs a moment to settle after its drivers turn
 * on, so each channel is read SAMPLES+1 times and the first read is thrown
 * away; the rest are averaged.
 */

#include "board.h"
#include "spi.h"
#include "touch.h"

#define CMD_X    0xD0
#define CMD_Y    0x90
#define CMD_Z1   0xB0
#define CMD_Z2   0xC0

#define SAMPLES  4

#define CS_LOW()   (TOUCH_PORT &= ~(1 << TOUCH_CS))
#define CS_HIGH()  (TOUCH_PORT |=  (1 << TOUCH_CS))

/*
 * touch_init - touch CS idles high. spi_init() has already made PD7 an
 * output and driven it high; this is kept explicit for clarity.
 */
void touch_init(void)
{
    TOUCH_PORT |= (1 << TOUCH_CS);
    TOUCH_DDR  |= (1 << TOUCH_CS);
}

/*
 * convert - send one control byte and return the 12-bit result.
 * CS must already be low and SCK at touch speed.
 */
static uint16_t convert(uint8_t cmd)
{
    spi_xfer(cmd);
    uint8_t hi = spi_xfer(0x00);
    uint8_t lo = spi_xfer(0x00);
    return (uint16_t)(((uint16_t)hi << 8) | lo) >> 3;
}

/*
 * channel - first conversion discarded (settling), then SAMPLES averaged.
 */
static uint16_t channel(uint8_t cmd)
{
    uint16_t sum = 0;
    convert(cmd);
    for (uint8_t i = 0; i < SAMPLES; i++) {
        sum += convert(cmd);
    }
    return sum / SAMPLES;
}

/*
 * touch_read_raw - read X, Y, Z1, Z2. Returns 1 if the panel is pressed
 * (Z1 above TOUCH_Z_MIN), else 0. The values are filled in either way.
 */
uint8_t touch_read_raw(touch_raw_t *r)
{
    spi_speed_touch();
    CS_LOW();
    r->z1 = channel(CMD_Z1);
    r->z2 = channel(CMD_Z2);
    r->x  = channel(CMD_X);
    r->y  = channel(CMD_Y);
    /* Finish with PD = 00 (the commands above already end in 00), so the
     * chip powers down with PENIRQ enabled between reads. */
    CS_HIGH();
    spi_speed_display();

    return (r->z1 >= TOUCH_Z_MIN);
}

/*
 * map_axis - linear map of a raw reading to 0..span, from the raw values
 * at the two screen edges (r0 at 0, r1 at span), clamped to the screen.
 */
static int16_t map_axis(uint16_t raw, int16_t r0, int16_t r1, int16_t span)
{
    int32_t v = ((int32_t)raw - r0) * span / (r1 - r0);
    if (v < 0) v = 0;
    if (v > span) v = span;
    return (int16_t)v;
}

/*
 * touch_read - screen coordinates (0..319, 0..239) of the current touch.
 * Returns 1 if pressed, else 0 (sx, sy untouched).
 */
uint8_t touch_read(int16_t *sx, int16_t *sy)
{
    touch_raw_t r;
    if (!touch_read_raw(&r)) {
        return 0;
    }
#if TOUCH_SWAP_XY
    uint16_t rx = r.y, ry = r.x;
#else
    uint16_t rx = r.x, ry = r.y;
#endif
    *sx = map_axis(rx, TOUCH_X0, TOUCH_X1, 319);
    *sy = map_axis(ry, TOUCH_Y0, TOUCH_Y1, 239);
    return 1;
}
