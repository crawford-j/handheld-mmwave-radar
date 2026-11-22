/*
 * touch_cal.c - One-time touch calibration (`make TOUCH_CAL=1`).
 *
 * The XPT2046 reports raw 12-bit voltages, not pixels. How they map to the
 * screen depends on the panel and on the display rotation (MADCTL), so it
 * is measured once:
 *
 *   1. Show a crosshair at each corner, 20 px in from the edges:
 *        TL (20, 20)  TR (300, 20)  BR (300, 220)  BL (20, 220)
 *      Wait for a firm touch, average 16 readings, wait for release.
 *   2. Axis swap: whichever raw channel changes most between left and right
 *      targets runs along the screen's x axis.
 *   3. Edge values: average the two readings at each side, then extrapolate
 *      linearly from the targets (20 px in) to the screen edges:
 *        X0 = a20 - (a300 - a20) * 20 / 280     (raw at x = 0)
 *        X1 = a300 + (a300 - a20) * 19 / 280    (raw at x = 319)
 *      and the same for y over 200 px (20 -> 220).
 *   4. Show TOUCH_SWAP_XY / X0 / X1 / Y0 / Y1 to copy into touch.h, then a
 *      draw test: a dot appears wherever the panel is touched, mapped with
 *      the new values, so the result can be checked straight away.
 */

#include "board.h"
#include <avr/pgmspace.h>
#include <util/delay.h>
#include "ili9341.h"
#include "gfx.h"
#include "touch.h"
#include "touch_cal.h"

#define INSET       20
#define AVG_READS   16
#define CONFIRM     3       /* consecutive pressed reads to accept a touch */
#define RELEASE     5       /* consecutive released reads to accept a lift */

typedef struct {
    int16_t x, y;           /* raw averages */
} rawpt_t;

/*
 * crosshair - 21 px cross centered on (x, y).
 */
static void crosshair(int16_t x, int16_t y, uint16_t color)
{
    gfx_hline(x - 10, y, 21, color);
    gfx_vline(x, y - 10, 21, color);
    gfx_circle(x, y, 4, color);
}

/*
 * show_raw - live raw readout on the top line.
 */
static void show_raw(const touch_raw_t *r)
{
    int16_t x = gfx_text_P(4, 2, PSTR("X"), COLOR_GRAY, COLOR_BLACK, 1);
    x = gfx_int_pad(x, 2, r->x, 5, COLOR_WHITE, COLOR_BLACK, 1);
    x = gfx_text_P(x, 2, PSTR("  Y"), COLOR_GRAY, COLOR_BLACK, 1);
    x = gfx_int_pad(x, 2, r->y, 5, COLOR_WHITE, COLOR_BLACK, 1);
    x = gfx_text_P(x, 2, PSTR("  Z1"), COLOR_GRAY, COLOR_BLACK, 1);
    x = gfx_int_pad(x, 2, r->z1, 5, COLOR_WHITE, COLOR_BLACK, 1);
    x = gfx_text_P(x, 2, PSTR("  Z2"), COLOR_GRAY, COLOR_BLACK, 1);
    gfx_int_pad(x, 2, r->z2, 5, COLOR_WHITE, COLOR_BLACK, 1);
}

/*
 * wait_touch - wait for a firm touch, average AVG_READS readings, then wait
 * for the stylus to lift. Shows the live raw values while waiting.
 */
static rawpt_t wait_touch(void)
{
    touch_raw_t r;
    uint8_t n = 0;

    while (n < CONFIRM) {
        uint8_t down = touch_read_raw(&r);
        show_raw(&r);
        n = down ? n + 1 : 0;
        _delay_ms(10);
    }

    int32_t sx = 0, sy = 0;
    uint8_t got = 0;
    while (got < AVG_READS) {
        if (touch_read_raw(&r)) {
            sx += r.x;
            sy += r.y;
            got++;
        }
        _delay_ms(5);
    }

    n = 0;
    while (n < RELEASE) {
        n = touch_read_raw(&r) ? 0 : n + 1;
        _delay_ms(10);
    }

    rawpt_t p = { (int16_t)(sx / AVG_READS), (int16_t)(sy / AVG_READS) };
    return p;
}

static int16_t iabs(int16_t v)
{
    return (v < 0) ? -v : v;
}

/*
 * map_axis - same linear map as touch.c, with runtime constants.
 */
static int16_t map_axis(int16_t raw, int16_t r0, int16_t r1, int16_t span)
{
    if (r1 == r0) {
        return 0;
    }
    int32_t v = ((int32_t)raw - r0) * span / (r1 - r0);
    if (v < 0) v = 0;
    if (v > span) v = span;
    return (int16_t)v;
}

/*
 * show_const - one result line, "NAME value", at scale 2.
 */
static void show_const(int16_t y, const char *name, int16_t value)
{
    int16_t x = gfx_text_P(40, y, name, COLOR_CYAN, COLOR_BLACK, 2);
    gfx_int_pad(x, y, value, 6, COLOR_YELLOW, COLOR_BLACK, 2);
}

void touch_cal_run(void)
{
    static const int16_t tx[4] = { INSET, LCD_WIDTH - INSET, LCD_WIDTH - INSET, INSET };
    static const int16_t ty[4] = { INSET, INSET, LCD_HEIGHT - INSET, LCD_HEIGHT - INSET };
    rawpt_t p[4];

    touch_init();
    gfx_fill_screen(COLOR_BLACK);
    gfx_text_P(70, 100, PSTR("TOUCH CALIBRATION"), COLOR_YELLOW, COLOR_BLACK, 1);
    gfx_text_P(40, 116, PSTR("TAP THE CENTER OF EACH CROSS"), COLOR_WHITE, COLOR_BLACK, 1);
    gfx_text_P(64, 128, PSTR("FIRMLY, WITH THE STYLUS"), COLOR_WHITE, COLOR_BLACK, 1);

    /* 1. Collect the four corners. */
    for (uint8_t i = 0; i < 4; i++) {
        crosshair(tx[i], ty[i], COLOR_WHITE);
        p[i] = wait_touch();
        crosshair(tx[i], ty[i], COLOR_GREEN);
    }

    /* 2. Which raw channel runs along screen x? TL->TR and BL->BR are the
     *    left-to-right pairs. */
    int16_t dx_rawx = iabs(p[1].x - p[0].x) + iabs(p[2].x - p[3].x);
    int16_t dx_rawy = iabs(p[1].y - p[0].y) + iabs(p[2].y - p[3].y);
    uint8_t swap = (dx_rawy > dx_rawx);

    /* a = channel along screen x, b = channel along screen y. */
    int16_t a[4], b[4];
    for (uint8_t i = 0; i < 4; i++) {
        a[i] = swap ? p[i].y : p[i].x;
        b[i] = swap ? p[i].x : p[i].y;
    }

    /* 3. Average each side, extrapolate to the screen edges. */
    int32_t a_left  = (a[0] + a[3]) / 2;        /* x = 20  */
    int32_t a_right = (a[1] + a[2]) / 2;        /* x = 300 */
    int32_t b_top   = (b[0] + b[1]) / 2;        /* y = 20  */
    int32_t b_bot   = (b[2] + b[3]) / 2;        /* y = 220 */

    int16_t x0 = (int16_t)(a_left  - (a_right - a_left) * INSET / 280);
    int16_t x1 = (int16_t)(a_right + (a_right - a_left) * (INSET - 1) / 280);
    int16_t y0 = (int16_t)(b_top   - (b_bot - b_top) * INSET / 200);
    int16_t y1 = (int16_t)(b_bot   + (b_bot - b_top) * (INSET - 1) / 200);

    /* 4. Show the constants, then the draw test. */
    gfx_fill_screen(COLOR_BLACK);
    gfx_text_P(40, 20, PSTR("COPY INTO touch.h:"), COLOR_WHITE, COLOR_BLACK, 1);
    show_const(40,  PSTR("SWAP_XY"), swap);
    show_const(64,  PSTR("X0     "), x0);
    show_const(88,  PSTR("X1     "), x1);
    show_const(112, PSTR("Y0     "), y0);
    show_const(136, PSTR("Y1     "), y1);
    gfx_text_P(40, 170, PSTR("NOW DRAW: DOTS SHOULD FOLLOW"), COLOR_GREEN, COLOR_BLACK, 1);
    gfx_text_P(40, 182, PSTR("THE STYLUS. RESET TO REDO."), COLOR_GREEN, COLOR_BLACK, 1);

    for (;;) {
        touch_raw_t r;
        if (touch_read_raw(&r)) {
            int16_t ra = swap ? r.y : r.x;
            int16_t rb = swap ? r.x : r.y;
            int16_t sx = map_axis(ra, x0, x1, LCD_WIDTH - 1);
            int16_t sy = map_axis(rb, y0, y1, LCD_HEIGHT - 1);
            ili9341_fill_rect(sx - 1, sy - 1, 3, 3, COLOR_MAGENTA);
        }
    }
}
