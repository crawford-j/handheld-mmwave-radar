/*
 * ui.c - Radar view on the 320x240 landscape display.
 *
 * Layout: a 120-degree fan (+/-60 deg, the LD2450's azimuth coverage
 * [LD2450 manual §2.1]) with its apex at the bottom center of the screen.
 *   apex (160, 230), radius 180 px = 6 m (the radar's maximum range)
 *   scale: 6000 mm / 180 px = 33.3 mm per pixel
 *   fan edges end at 180*sin60 = 156 px left/right and 180*cos60 = 90 px up,
 *   so the fan spans x = 4..316, y = 50..230 and just fits the screen.
 *
 * No framebuffer: the static background (rings, edges, center line) is never
 * stored. bg_color_at(x, y) computes what belongs at any pixel, and is used
 * both to draw the background and to repair it after a ping, label or sweep
 * line moves. Drawing and repairing use the same function, so a repair never
 * leaves stray pixels.
 */

#include <avr/pgmspace.h>
#include "ili9341.h"
#include "gfx.h"
#include "font5x7.h"
#include "trig.h"
#include "ui.h"

/* ---- Geometry ---- */
#define APEX_X      160
#define APEX_Y      230
#define R_PX        180             /* outer ring radius in pixels */
#define RANGE_MM    6000L           /* distance at the outer ring  */
#define EDGE_DX     156             /* fan edge end: 180 * sin(60) */
#define EDGE_DY     90              /* fan edge end: 180 * cos(60) */

#define SWEEP_MAX   58              /* sweep turns back 2 deg inside the edges */
#define SWEEP_STEP  2               /* degrees per step                        */
#define SWEEP_R     (R_PX - 2)

#define N_RINGS     4
static const uint8_t ring_px[N_RINGS] PROGMEM = { 45, 90, 135, 180 };  /* 1.5 m steps */
static const char ring_lbl0[] PROGMEM = "1.5m";
static const char ring_lbl1[] PROGMEM = "3m";
static const char ring_lbl2[] PROGMEM = "4.5m";
static const char ring_lbl3[] PROGMEM = "6m";
static const char *const ring_lbl[N_RINGS] PROGMEM = { ring_lbl0, ring_lbl1, ring_lbl2, ring_lbl3 };

/* ---- Colors ---- */
#define C_BG        COLOR_BLACK
#define C_RING      RGB565(0, 110, 0)
#define C_OUTER     RGB565(0, 200, 0)
#define C_EDGE      RGB565(0, 110, 0)
#define C_CENTER    RGB565(0, 50, 0)
#define C_SWEEP     RGB565(80, 255, 80)
#define C_LABEL     RGB565(0, 160, 0)
#define C_STATUS    COLOR_GRAY

static const uint16_t ping_color[RADAR_TARGETS] = { COLOR_YELLOW, COLOR_CYAN, COLOR_MAGENTA };

#define PING_HALF   2               /* ping is a 5x5 square */
#define LABEL_MAX   6               /* "6.0m" + spare */

/* What is currently drawn for each target slot, so it can be erased. */
typedef struct {
    uint8_t on;
    int16_t px, py;                 /* ping center         */
    int16_t lx, ly;                 /* label top-left      */
    uint8_t len;                    /* label length (chars) */
    char    text[LABEL_MAX + 1];
} ping_t;

static ping_t pings[RADAR_TARGETS];

static int16_t sweep_deg = -SWEEP_MAX;
static int8_t  sweep_dir = 1;
static int16_t sweep_ex, sweep_ey;  /* end point of the line on screen */
static uint8_t sweep_drawn;

/*
 * bg_color_at - the background color that belongs at screen pixel (x, y).
 *
 * Work relative to the apex: dx to the right, dy upward, d^2 = dx^2 + dy^2.
 *   outside the outer ring or below the apex -> black
 *   fan edge: the 60-degree line from the apex. Its x extent (156) is larger
 *     than its y extent (90), so it has one pixel per column; a pixel is on
 *     it when |dx*90/156 - dy| <= 0.5, i.e. |dx*90 - dy*156| <= 78
 *   outside the edge (e > 78) -> black
 *   ring of radius r: pixels with r - 0.5 <= d <= r + 0.5, which is
 *     |d^2 - r^2| <= r to within a quarter pixel
 *   center line: dx == 0
 */
static uint16_t bg_color_at(int16_t x, int16_t y)
{
    int16_t dx = x - APEX_X;
    int16_t dy = APEX_Y - y;
    if (dy < 0) {
        return C_BG;
    }

    int32_t d2 = (int32_t)dx * dx + (int32_t)dy * dy;
    if (d2 > (int32_t)R_PX * R_PX + R_PX) {
        return C_BG;
    }

    int16_t adx = (dx < 0) ? -dx : dx;
    int32_t e = (int32_t)adx * EDGE_DY - (int32_t)dy * EDGE_DX;
    if (e > EDGE_DX / 2) {
        return C_BG;                    /* outside the fan */
    }
    if (e >= -(EDGE_DX / 2)) {
        return C_EDGE;                  /* on the fan edge */
    }

    for (uint8_t i = 0; i < N_RINGS; i++) {
        int32_t r  = pgm_read_byte(&ring_px[i]);
        int32_t r2 = r * r;
        if (d2 >= r2 - r && d2 <= r2 + r) {
            return (i == N_RINGS - 1) ? C_OUTER : C_RING;
        }
    }

    if (dx == 0) {
        return C_CENTER;
    }
    return C_BG;
}

/*
 * restore_rect - repaint a rectangle with its background, streaming every
 * pixel from bg_color_at() through one address window.
 */
static void restore_rect(int16_t x, int16_t y, int16_t w, int16_t h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH)  w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) {
        return;
    }

    ili9341_set_window(x, y, x + w - 1, y + h - 1);
    for (int16_t yy = y; yy < y + h; yy++) {
        for (int16_t xx = x; xx < x + w; xx++) {
            ili9341_push_color(bg_color_at(xx, yy), 1);
        }
    }
    ili9341_end();
}

/*
 * draw_label - draw text whose "transparent" pixels show the radar
 * background (rings stay visible behind labels). `s` is in RAM, or in flash
 * if `in_flash` is set. Must fit fully on screen.
 */
static void draw_label(int16_t x, int16_t y, const char *s, uint8_t in_flash, uint16_t fg)
{
    uint8_t len = 0;
    while ((in_flash ? pgm_read_byte(&s[len]) : (uint8_t)s[len]) != '\0') {
        len++;
    }
    if (len == 0) {
        return;
    }

    int16_t w = len * GFX_CHAR_W;
    ili9341_set_window(x, y, x + w - 1, y + GFX_CHAR_H - 1);
    for (uint8_t row = 0; row < GFX_CHAR_H; row++) {
        for (uint8_t i = 0; i < len; i++) {
            char c = in_flash ? (char)pgm_read_byte(&s[i]) : s[i];
            if (c < FONT_FIRST || c > FONT_LAST) {
                c = '?';
            }
            const uint8_t *glyph = font5x7[c - FONT_FIRST];
            for (uint8_t col = 0; col < GFX_CHAR_W; col++) {
                uint8_t bits = (col < FONT_W) ? pgm_read_byte(&glyph[col]) : 0;
                uint16_t color = (bits & (1 << row))
                               ? fg
                               : bg_color_at(x + i * GFX_CHAR_W + col, y + row);
                ili9341_push_color(color, 1);
            }
        }
    }
    ili9341_end();
}

/* Ring label i: top-left just above its ring, right of the center line. */
#define RING_LBL_X        (APEX_X + 3)
#define RING_LBL_Y(r)     (APEX_Y - (r) - GFX_CHAR_H - 1)
#define RING_LBL_W        (4 * GFX_CHAR_W)      /* longest label, "4.5m" */

/*
 * draw_ring_label - draw the range label for ring i.
 */
static void draw_ring_label(uint8_t i)
{
    int16_t r = pgm_read_byte(&ring_px[i]);
    const char *lbl = (const char *)pgm_read_word(&ring_lbl[i]);
    draw_label(RING_LBL_X, RING_LBL_Y(r), lbl, 1, C_LABEL);
}

/*
 * draw_ring_labels - all range labels. Redrawn after pings move, since an
 * erased ping can cut into a label.
 */
static void draw_ring_labels(void)
{
    for (uint8_t i = 0; i < N_RINGS; i++) {
        draw_ring_label(i);
    }
}

/*
 * ring_label_near_sweep - true if the line at `deg` may cross label i.
 */
static uint8_t near_line(int16_t x, int16_t y, int16_t deg, int16_t tol);
static uint8_t ring_label_near_sweep(uint8_t i, int16_t deg)
{
    int16_t r = pgm_read_byte(&ring_px[i]);
    return near_line(RING_LBL_X + RING_LBL_W / 2, RING_LBL_Y(r) + GFX_CHAR_H / 2,
                     deg, RING_LBL_W / 2 + 4);
}

/*
 * draw_ping - draw one target's square and distance label.
 */
static void draw_ping(uint8_t n)
{
    const ping_t *p = &pings[n];
    ili9341_fill_rect(p->px - PING_HALF, p->py - PING_HALF,
                      2 * PING_HALF + 1, 2 * PING_HALF + 1, ping_color[n]);
    draw_label(p->lx, p->ly, p->text, 0, ping_color[n]);
}

/*
 * erase_ping - restore the background under a target's square and label.
 */
static void erase_ping(uint8_t n)
{
    const ping_t *p = &pings[n];
    restore_rect(p->px - PING_HALF, p->py - PING_HALF, 2 * PING_HALF + 1, 2 * PING_HALF + 1);
    restore_rect(p->lx, p->ly, p->len * GFX_CHAR_W, GFX_CHAR_H);
}

/*
 * format_meters - write a distance as "d.dm" (tenths of a meter, rounded).
 * Returns the string length.
 */
static uint8_t format_meters(char *buf, uint16_t mm)
{
    uint16_t tenths = (mm + 50) / 100;
    uint8_t i = 0;
    uint16_t whole = tenths / 10;

    if (whole >= 10) {
        buf[i++] = '0' + whole / 10;
    }
    buf[i++] = '0' + whole % 10;
    buf[i++] = '.';
    buf[i++] = '0' + tenths % 10;
    buf[i++] = 'm';
    buf[i] = '\0';
    return i;
}

/*
 * ui_radar_init - clear the screen and draw the radar background.
 * The fan's bounding box (313 x 181 px) is streamed from bg_color_at().
 */
void ui_radar_init(void)
{
    gfx_fill_screen(C_BG);
    restore_rect(APEX_X - EDGE_DX, APEX_Y - R_PX, 2 * EDGE_DX + 1, R_PX + 1);
    draw_ring_labels();

    gfx_text_P(4, 14, PSTR("RADAR"), COLOR_YELLOW, C_BG, 2);

    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        pings[n].on = 0;
    }
    sweep_drawn = 0;
}

/*
 * ui_radar_targets - move the pings to the targets in a new frame.
 *
 * A target is shown if the slot is valid, it is at least MIN_RANGE_MM away
 * (closer is the person holding the device), no farther than the outer ring,
 * and inside the fan. All old pings are erased before any new one is drawn,
 * so erasing one never cuts into another.
 */
void ui_radar_targets(const radar_frame_t *fr)
{
    ping_t next[RADAR_TARGETS];

    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        const radar_target_t *t = &fr->t[n];
        ping_t *q = &next[n];
        q->on = 0;
        if (!t->valid) {
            continue;
        }

        int32_t x = t->x;
        int32_t y = t->y;
        uint16_t d = isqrt32((uint32_t)(x * x) + (uint32_t)(y * y));
        if (d < MIN_RANGE_MM || d > RANGE_MM) {
            continue;
        }

        /* mm -> px: 180 px per 6000 mm, X mirrored if RADAR_X_SIGN = -1. */
        int16_t dx = (int16_t)((RADAR_X_SIGN * x * R_PX) / RANGE_MM);
        int16_t dy = (int16_t)((y * R_PX) / RANGE_MM);
        int16_t adx = (dx < 0) ? -dx : dx;
        if (dy <= 0 || (int32_t)adx * EDGE_DY > (int32_t)dy * EDGE_DX) {
            continue;                   /* behind the radar or outside the fan */
        }

        q->px = APEX_X + dx;
        q->py = APEX_Y - dy;
        q->len = format_meters(q->text, d);

        /* Label to the right of the ping, or to the left near the right edge. */
        int16_t w = q->len * GFX_CHAR_W;
        q->lx = q->px + PING_HALF + 3;
        if (q->lx + w > LCD_WIDTH) {
            q->lx = q->px - PING_HALF - 3 - w;
        }
        q->ly = q->py - GFX_CHAR_H / 2;
        if (q->ly > LCD_HEIGHT - GFX_CHAR_H) {
            q->ly = LCD_HEIGHT - GFX_CHAR_H;
        }
        q->on = 1;
    }

    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        if (pings[n].on) {
            erase_ping(n);
        }
    }
    draw_ring_labels();
    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        pings[n] = next[n];
        if (pings[n].on) {
            draw_ping(n);
        }
    }
}

/*
 * near_line - true if screen point (x, y) is within `tol` px of the sweep
 * line at angle `deg`, so the line may have drawn over something there.
 * Perpendicular distance from the line through the apex at angle a:
 *   |dx*cos(a) - dy*sin(a)|, with sin/cos in Q15 (>> 15 to get pixels).
 */
static uint8_t near_line(int16_t x, int16_t y, int16_t deg, int16_t tol)
{
    int32_t dx = x - APEX_X;
    int32_t dy = APEX_Y - y;
    int32_t perp = (dx * cos_q15(deg) - dy * sin_q15(deg)) >> 15;
    if (perp < 0) {
        perp = -perp;
    }
    return perp <= tol;
}

/*
 * ping_near_sweep - true if slot n's ping or label may have been crossed by
 * the line at `deg`. Tested at the middle of the ping+label box, with a
 * tolerance of half the box width plus 2 px.
 */
static uint8_t ping_near_sweep(uint8_t n, int16_t deg)
{
    const ping_t *p = &pings[n];
    int16_t left  = (p->lx < p->px - PING_HALF) ? p->lx : p->px - PING_HALF;
    int16_t right = (p->lx + p->len * GFX_CHAR_W > p->px + PING_HALF)
                  ? p->lx + p->len * GFX_CHAR_W : p->px + PING_HALF;
    return near_line((left + right) / 2, p->py, deg, (right - left) / 2 + 2);
}

/*
 * ui_sweep_step - move the sweep line SWEEP_STEP degrees, bouncing between
 * -SWEEP_MAX and +SWEEP_MAX.
 *   1. erase the old line by restoring the background under each pixel
 *   2. draw the new line
 *   3. redraw ring labels if the line is near the center, and any pings
 *      the old or new line passed over, so they stay on top
 */
void ui_sweep_step(void)
{
    int16_t old_deg = sweep_deg;

    if (sweep_drawn) {
        gfx_line_fn(APEX_X, APEX_Y, sweep_ex, sweep_ey, bg_color_at);
    }

    sweep_deg += sweep_dir * SWEEP_STEP;
    if (sweep_deg >= SWEEP_MAX) {
        sweep_deg = SWEEP_MAX;
        sweep_dir = -1;
    } else if (sweep_deg <= -SWEEP_MAX) {
        sweep_deg = -SWEEP_MAX;
        sweep_dir = 1;
    }

    /* End point: x = r sin(a) to the right, y = r cos(a) upward. */
    sweep_ex = APEX_X + (int16_t)(((int32_t)SWEEP_R * sin_q15(sweep_deg)) >> 15);
    sweep_ey = APEX_Y - (int16_t)(((int32_t)SWEEP_R * cos_q15(sweep_deg)) >> 15);
    gfx_line(APEX_X, APEX_Y, sweep_ex, sweep_ey, C_SWEEP);
    sweep_drawn = 1;

    for (uint8_t i = 0; i < N_RINGS; i++) {
        if (ring_label_near_sweep(i, old_deg) || ring_label_near_sweep(i, sweep_deg)) {
            draw_ring_label(i);
        }
    }
    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        if (pings[n].on && (ping_near_sweep(n, old_deg) || ping_near_sweep(n, sweep_deg))) {
            draw_ping(n);
        }
    }
}

/*
 * ui_status - debug line at the top: frame count, errors and frame rate.
 */
void ui_status(const radar_stats_t *s, uint16_t hz)
{
    int16_t x = gfx_text_P(4, 2, PSTR("FR"), C_STATUS, C_BG, 1);
    x = gfx_int_pad(x, 2, s->frames_ok, 6, COLOR_WHITE, C_BG, 1);
    x = gfx_text_P(x, 2, PSTR("  BAD"), C_STATUS, C_BG, 1);
    x = gfx_int_pad(x, 2, s->frames_bad, 4, s->frames_bad ? COLOR_RED : COLOR_WHITE, C_BG, 1);
    x = gfx_text_P(x, 2, PSTR("  FE"), C_STATUS, C_BG, 1);
    x = gfx_int_pad(x, 2, s->fe, 4, s->fe ? COLOR_RED : COLOR_WHITE, C_BG, 1);
    x = gfx_text_P(x, 2, PSTR("  OVF"), C_STATUS, C_BG, 1);
    x = gfx_int_pad(x, 2, s->ovf + s->dor, 4, (s->ovf + s->dor) ? COLOR_RED : COLOR_WHITE,
                    C_BG, 1);
    x = gfx_int_pad(x, 2, hz, 4, COLOR_WHITE, C_BG, 1);
    gfx_text_P(x, 2, PSTR("HZ"), C_STATUS, C_BG, 1);
}
