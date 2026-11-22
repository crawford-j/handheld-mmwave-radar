/*
 * ui.c - Radar view on the 320x240 landscape display.
 *
 * Layout: a filled half-dome with its apex at the bottom center.
 *   apex (160, 200), radius 156 px = 6 m (the radar's maximum range)
 *   scale: 6000 mm / 156 px = 38.5 mm per pixel
 *   the dome spans x = 4..316, y = 44..200
 * The LD2450 only sees +/-60 degrees [LD2450 manual §2.1, Fig. 6], so the
 * dome is drawn brighter inside that cone and dark outside it.
 *   top bar    y = 0..42   debug status and readout
 *   dome       y = 44..200
 *   baseline   y = 203..210 range labels
 *   y = 213..228           big range text ("6M")
 *   y = 231..238           alert zone / ALERT line
 *
 * No framebuffer: the background is never stored. bg_color_at(x, y)
 * computes what belongs at any pixel (gradient fill, rings, edges, alert
 * ring) and is used both to draw the dome and to repair it after a ping,
 * label, arrow or the pulse moves. Drawing and repairing use the same
 * function, so a repair never leaves stray pixels.
 *
 * Scan animation (UI_SCAN_PULSE, default 1): a ring expanding from the apex
 * to the edge, like a sonar pulse, drawn only inside the +/-60 degree cone
 * the radar covers. Its radius comes from the clock (PULSE_PX_PER_S), so
 * it keeps a steady speed even if a drawing step runs long. With
 * UI_SCAN_PULSE = 0 a sweep line bounces across the cone instead.
 *
 * Pings glide: each target has a goal (from track.c, ~11 updates/s) and a
 * drawn position that moves halfway to the goal on every animation tick, so
 * dots slide instead of jumping between radar frames.
 */

#include <avr/pgmspace.h>
#include "ili9341.h"
#include "gfx.h"
#include "font5x7.h"
#include "trig.h"
#include "track.h"
#include "alert.h"
#include "ui.h"

/* ---- Geometry ---- */
#define APEX_X      160
#define APEX_Y      200
#define R_PX        156             /* dome radius in pixels = RANGE_MM */
#define RANGE_MM    6000L
#define COV_DX      173             /* cone edge direction: tan(60) = 173/100 */
#define COV_DY      100

#define N_RINGS     3
static const uint8_t ring_px[N_RINGS] PROGMEM = { 39, 78, 117 };   /* 1.5, 3, 4.5 m */
static const char lbl0[] PROGMEM = "1.5";
static const char lbl1[] PROGMEM = "3";
static const char lbl2[] PROGMEM = "4.5";
static const char lbl3[] PROGMEM = "6";
static const char *const range_lbl[N_RINGS + 1] PROGMEM = { lbl0, lbl1, lbl2, lbl3 };

/* Pulse: radius = PULSE_MIN + elapsed * PULSE_PX_PER_S, restarting at the
 * rim: (153 - 6) px / 70 px/s = 2.1 s per pulse. */
#define PULSE_MIN       6
#define PULSE_PX_PER_S  70

/* Sweep (UI_SCAN_PULSE = 0): bounces inside the cone. */
#define SWEEP_MAX   58
#define SWEEP_STEP  2
#define SWEEP_R     (R_PX - 3)

/* ---- Colors ---- */
#define GREEN6(g)   ((uint16_t)((g) & 0x3F) << 5)   /* RGB565 with only green set */
#define C_BG        COLOR_BLACK
#define C_EDGE      RGB565(80, 255, 80)     /* bright outer rim               */
#define C_RING      RGB565(60, 200, 60)     /* dotted range rings             */
#define C_LINE      RGB565(120, 230, 120)   /* dashed baseline / center line  */
#define C_CONE      RGB565(40, 140, 40)     /* +/-60 degree coverage edges    */
#define C_ALERT     RGB565(230, 30, 30)     /* alert-zone ring                */
#define C_PULSE     RGB565(200, 255, 200)
#define C_SWEEP     RGB565(160, 255, 160)
#define C_TEXT      RGB565(150, 230, 150)
#define C_STATUS    COLOR_GRAY
#define C_PING      RGB565(190, 255, 190)
#define C_PING_HALO RGB565(60, 170, 60)
#define C_PING_HOT  RGB565(255, 70, 70)     /* inside the alert zone */
#define C_LABEL     COLOR_WHITE

#define PING_R      3               /* dot radius             */
#define HALO_R      5               /* glow ring radius       */
#define LABEL_MAX   6               /* "6.0m" + spare         */
#define LABEL_GAP   2               /* px between dot and label */
#define ARROW_LEN   14
#define ARROW_BACK  5
#define ARROW_WIDE  3

/* What is drawn (or should be drawn) for one target. */
typedef struct {
    uint8_t  on;
    int16_t  px, py;                /* dot center               */
    uint16_t color;                 /* dot color (green or red) */
    int16_t  lx, ly;                /* label top-left           */
    uint8_t  len;
    char     text[LABEL_MAX + 1];
    uint8_t  arrow;
    int16_t  ax, ay;                /* arrow tip                */
    int16_t  h1x, h1y, h2x, h2y;    /* arrow head ends          */
} ping_t;

/* Where a target should end up, set once per radar frame. */
typedef struct {
    uint8_t  want;
    int16_t  px, py;
    uint16_t color;
    int16_t  vx, vy;                /* motion direction, screen px (0 = none) */
    uint8_t  len;
    char     text[LABEL_MAX + 1];
} goal_t;

static ping_t pings[RADAR_TARGETS];
static goal_t goals[RADAR_TARGETS];

static int16_t  pulse_r;            /* radius of the drawn pulse ring, 0 = none */
#if UI_SCAN_PULSE
static uint32_t pulse_t0;           /* when the current pulse started           */
#endif
#if !UI_SCAN_PULSE
static int16_t sweep_deg = -SWEEP_MAX;
static int8_t  sweep_dir = 1;
static int16_t sweep_ex, sweep_ey;
#endif
static uint8_t sweep_drawn;

static int16_t alert_r_px;          /* alert-zone ring radius, px (0 = none)   */
static uint8_t alert_shown = 0xFF;  /* ALERT text state on screen (0xFF = none) */

/* ---------------------------------------------------------------------- */
/* Background                                                              */
/* ---------------------------------------------------------------------- */

/*
 * bg_color_at - the background color that belongs at screen pixel (x, y).
 *
 * Relative to the apex: dx to the right, dy upward, d^2 = dx^2 + dy^2,
 * R = dome radius. In order of priority:
 *   below the baseline or outside R          -> black
 *   rim: d in about [R-1.5, R+0.5]           -> bright edge (2 px thick)
 *   baseline (dy == 0): dashes, 4 px on/off  -> line color
 *   alert ring at the alert radius           -> red
 *   range ring: |d^2 - r^2| <= r (d within half a pixel of r), dotted by
 *     drawing only where (x + y) & 2         -> ring color
 *   center line (dx == 0): dashes            -> line color
 *   cone edge: the 60-degree line, |dx*100 - dy*173| <= 86 (x is the major
 *     axis there, so one pixel per column)   -> cone color
 *   fill: green that brightens with d^2/R^2 (cheap radial gradient);
 *     outside the cone (|dx|*100 > dy*173) the same at one third
 */
static uint16_t bg_color_at(int16_t x, int16_t y)
{
    int16_t dx = x - APEX_X;
    int16_t dy = APEX_Y - y;
    if (dy < 0) {
        return C_BG;
    }

    int32_t d2 = (int32_t)dx * dx + (int32_t)dy * dy;
    const int32_t R2 = (int32_t)R_PX * R_PX;
    if (d2 > R2 + R_PX) {
        return C_BG;
    }
    if (d2 >= (int32_t)(R_PX - 1) * (R_PX - 1) - (R_PX - 1)) {
        return C_EDGE;
    }

    int16_t adx = (dx < 0) ? -dx : dx;
    if (dy == 0) {
        return ((adx >> 2) & 1) ? C_LINE : C_BG;
    }

    if (alert_r_px) {
        int32_t r = alert_r_px;
        if (d2 >= r * r - r && d2 <= r * r + r) {
            return C_ALERT;
        }
    }

    for (uint8_t i = 0; i < N_RINGS; i++) {
        int32_t r = pgm_read_byte(&ring_px[i]);
        if (d2 >= r * r - r && d2 <= r * r + r) {
            if ((x + y) & 2) {
                return C_RING;
            }
            break;
        }
    }

    if (dx == 0) {
        return ((dy >> 2) & 1) ? C_LINE : C_BG;
    }

    /* |adx*100 - dy*173| <= 86 (= 173/2): within half a pixel of the edge. */
    int32_t e = (int32_t)adx * COV_DY - (int32_t)dy * COV_DX;
    if (e >= -(COV_DX / 2) && e <= COV_DX / 2) {
        return C_CONE;
    }

    uint8_t g = (uint8_t)(6 + (d2 * 22) / R2);      /* 6..28 of 63 */
    if (e > 0) {
        g = g / 3 + 1;                               /* outside the cone */
    }
    return GREEN6(g);
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
 * draw_label - text whose non-glyph pixels show the background, so the
 * dome stays visible behind labels. `s` in RAM, or flash if in_flash.
 */
static void draw_label(int16_t x, int16_t y, const char *s, uint8_t in_flash, uint16_t fg)
{
    uint8_t len = 0;
    while ((in_flash ? pgm_read_byte(&s[len]) : (uint8_t)s[len]) != '\0') {
        len++;
    }
    if (len == 0 || x < 0 || y < 0 ||
        x + len * GFX_CHAR_W > LCD_WIDTH || y + GFX_CHAR_H > LCD_HEIGHT) {
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

#if UI_SCAN_PULSE
/*
 * span - fill a 1-pixel-wide horizontal or vertical run (x0 <= x1,
 * y0 <= y1) with `color`, or with fn(x, y) per pixel if fn is set, through
 * a single address window. One window costs 11 bytes of commands; a lone
 * pixel costs 13 bytes in total, so runs save most of that per pixel.
 */
static void span(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                 uint16_t color, gfx_color_fn fn)
{
    if (x0 < 0 || y0 < 0 || x1 >= LCD_WIDTH || y1 >= LCD_HEIGHT) {
        return;
    }
    ili9341_set_window(x0, y0, x1, y1);
    if (!fn) {
        ili9341_push_color(color, (uint16_t)((x1 - x0 + 1) * (y1 - y0 + 1)));
    } else {
        for (int16_t yy = y0; yy <= y1; yy++) {
            for (int16_t xx = x0; xx <= x1; xx++) {
                ili9341_push_color(fn(xx, yy), 1);
            }
        }
    }
    ili9341_end();
}

/*
 * arc_flush - draw the points of one midpoint-circle column x, for
 * y = ya..yb, in the four upper octants:
 *   top octants  (APEX_X +/- y, APEX_Y - x): one row  -> horizontal runs
 *   side octants (APEX_X +/- x, APEX_Y - y): one column -> vertical runs,
 *     clipped to the cone: x*100 <= y*173, i.e. y >= ceil(x*100/173)
 * The top octants are within 45 degrees of straight up, so always inside
 * the +/-60 degree cone.
 */
static void arc_flush(int16_t x, int16_t ya, int16_t yb, uint16_t color, gfx_color_fn fn)
{
    int16_t row = APEX_Y - x;
    span(APEX_X + ya, row, APEX_X + yb, row, color, fn);
    span(APEX_X - yb, row, APEX_X - ya, row, color, fn);

    int16_t ymin = (int16_t)(((int32_t)x * COV_DY + COV_DX - 1) / COV_DX);
    int16_t lo = (ya > ymin) ? ya : ymin;
    if (lo <= yb) {
        span(APEX_X + x, APEX_Y - yb, APEX_X + x, APEX_Y - lo, color, fn);
        span(APEX_X - x, APEX_Y - yb, APEX_X - x, APEX_Y - lo, color, fn);
    }
}

/*
 * arc_walk - the part of a circle of radius r around the apex inside the
 * +/-60 degree cone, by the midpoint circle algorithm. Consecutive points
 * that share the same x (the algorithm's outer coordinate) are collected
 * into runs and drawn by arc_flush(), instead of one window per pixel.
 * Colors: `color`, or fn(x, y) per pixel if fn is set (used for erasing).
 */
static void arc_walk(int16_t r, uint16_t color, gfx_color_fn fn)
{
    int16_t x = r;
    int16_t y = 0;
    int16_t err = 1 - r;
    int16_t y_run = 0;                  /* first y of the current x */

    while (x >= y) {
        /* Next midpoint step. */
        int16_t nx = x;
        int16_t ny = y + 1;
        int16_t nerr;
        if (err < 0) {
            nerr = err + 2 * ny + 1;
        } else {
            nx = x - 1;
            nerr = err + 2 * (ny - nx) + 1;
        }

        /* x is about to change, or the octant ends: draw this x's run. */
        if (nx != x || ny > nx) {
            arc_flush(x, y_run, y, color, fn);
            y_run = ny;
        }
        x = nx;
        y = ny;
        err = nerr;
    }
}
#endif


/* ---------------------------------------------------------------------- */
/* Bounding boxes                                                          */
/* ---------------------------------------------------------------------- */

typedef struct {
    int16_t l, t, r, b;             /* inclusive */
} box_t;

static void box_add(box_t *bx, int16_t x, int16_t y)
{
    if (x < bx->l) bx->l = x;
    if (x > bx->r) bx->r = x;
    if (y < bx->t) bx->t = y;
    if (y > bx->b) bx->b = y;
}

/*
 * ping_box - bounding box of everything drawn for ping p.
 */
static box_t ping_box(const ping_t *p)
{
    box_t bx = { p->px - HALO_R, p->py - HALO_R, p->px + HALO_R, p->py + HALO_R };
    box_add(&bx, p->lx, p->ly);
    box_add(&bx, p->lx + p->len * GFX_CHAR_W - 1, p->ly + GFX_CHAR_H - 1);
    if (p->arrow) {
        box_add(&bx, p->ax, p->ay);
        box_add(&bx, p->h1x, p->h1y);
        box_add(&bx, p->h2x, p->h2y);
    }
    return bx;
}

#if UI_SCAN_PULSE
/*
 * box_near_arc - true if box bx may touch the circle of radius r around the
 * apex: compare the distance from the apex to the box center with r, using
 * half the box width + height as the tolerance.
 */
static uint8_t box_near_arc(const box_t *bx, int16_t r)
{
    int32_t cx = (bx->l + bx->r) / 2 - APEX_X;
    int32_t cy = APEX_Y - (bx->t + bx->b) / 2;
    int16_t d = isqrt32((uint32_t)(cx * cx + cy * cy));
    int16_t tol = (bx->r - bx->l) / 2 + (bx->b - bx->t) / 2 + 1;
    int16_t diff = d - r;
    return diff >= -tol && diff <= tol;
}
#endif


#if !UI_SCAN_PULSE
/*
 * box_near_line - true if box bx may touch the sweep line at angle `deg`
 * (perpendicular distance |dx*cos(a) - dy*sin(a)|, sin/cos in Q15).
 */
static uint8_t box_near_line(const box_t *bx, int16_t deg)
{
    int32_t dx = (bx->l + bx->r) / 2 - APEX_X;
    int32_t dy = APEX_Y - (bx->t + bx->b) / 2;
    int32_t perp = (dx * cos_q15(deg) - dy * sin_q15(deg)) >> 15;
    if (perp < 0) {
        perp = -perp;
    }
    return perp <= (bx->r - bx->l) / 2 + (bx->b - bx->t) / 2 + 1;
}
#endif


/* ---------------------------------------------------------------------- */
/* Pings                                                                   */
/* ---------------------------------------------------------------------- */

/*
 * draw_ping - arrow, glow ring, dot, then the distance label underneath.
 */
static void draw_ping(uint8_t n)
{
    const ping_t *p = &pings[n];

    if (p->arrow) {
        gfx_line(p->px, p->py, p->ax, p->ay, p->color);
        gfx_line(p->ax, p->ay, p->h1x, p->h1y, p->color);
        gfx_line(p->ax, p->ay, p->h2x, p->h2y, p->color);
    }
    gfx_circle(p->px, p->py, HALO_R, (p->color == C_PING) ? C_PING_HALO : p->color);
    gfx_fill_circle(p->px, p->py, PING_R, p->color);
    draw_label(p->lx, p->ly, p->text, 0, C_LABEL);
}

/*
 * erase_ping - restore the background under a ping's dot, label and arrow.
 */
static void erase_ping(uint8_t n)
{
    const ping_t *p = &pings[n];
    restore_rect(p->px - HALO_R, p->py - HALO_R, 2 * HALO_R + 1, 2 * HALO_R + 1);
    restore_rect(p->lx, p->ly, p->len * GFX_CHAR_W, GFX_CHAR_H);
    if (p->arrow) {
        gfx_line_fn(p->px, p->py, p->ax, p->ay, bg_color_at);
        gfx_line_fn(p->ax, p->ay, p->h1x, p->h1y, bg_color_at);
        gfx_line_fn(p->ax, p->ay, p->h2x, p->h2y, bg_color_at);
    }
}

/*
 * format_meters - distance as "d.dm" (tenths of a meter, rounded).
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
 * layout_ping - place the label (centered under the dot, kept on screen)
 * and the arrow for a dot at (px, py) with goal g.
 *
 * Arrow: the motion vector scaled to ARROW_LEN px; the head is two lines
 * from the tip, ARROW_BACK px back and ARROW_WIDE px to each side.
 */
static void layout_ping(ping_t *p, int16_t px, int16_t py, const goal_t *g)
{
    p->px = px;
    p->py = py;
    p->color = g->color;
    p->len = g->len;
    for (uint8_t i = 0; i <= g->len; i++) {
        p->text[i] = g->text[i];
    }

    int16_t w = p->len * GFX_CHAR_W;
    p->lx = px - w / 2;
    if (p->lx < 0) p->lx = 0;
    if (p->lx + w > LCD_WIDTH) p->lx = LCD_WIDTH - w;
    p->ly = py + HALO_R + LABEL_GAP;
    if (p->ly + GFX_CHAR_H > APEX_Y) {
        p->ly = py - HALO_R - LABEL_GAP - GFX_CHAR_H;   /* near the baseline: above */
    }

    p->arrow = (g->vx != 0 || g->vy != 0);
    if (p->arrow) {
        int32_t vx = g->vx, vy = g->vy;
        int16_t L = isqrt32((uint32_t)(vx * vx + vy * vy));
        int16_t dx = (int16_t)(vx * ARROW_LEN / L);
        int16_t dy = (int16_t)(vy * ARROW_LEN / L);
        p->ax = px + dx;
        p->ay = py + dy;
        int16_t bx = p->ax - dx * ARROW_BACK / ARROW_LEN;
        int16_t by = p->ay - dy * ARROW_BACK / ARROW_LEN;
        int16_t nx = -dy * ARROW_WIDE / ARROW_LEN;
        int16_t ny =  dx * ARROW_WIDE / ARROW_LEN;
        p->h1x = bx + nx;
        p->h1y = by + ny;
        p->h2x = bx - nx;
        p->h2y = by - ny;
    }
}

/*
 * same_ping - true if two ping states would draw identically.
 */
static uint8_t same_ping(const ping_t *a, const ping_t *b)
{
    if (a->on != b->on) return 0;
    if (!a->on) return 1;
    if (a->px != b->px || a->py != b->py || a->color != b->color ||
        a->arrow != b->arrow) return 0;
    if (a->arrow && (a->ax != b->ax || a->ay != b->ay)) return 0;
    for (uint8_t i = 0; i <= a->len; i++) {
        if (a->text[i] != b->text[i]) return 0;
    }
    return 1;
}

/*
 * glide - move `cur` halfway toward `goal`, snapping when within 1 px.
 */
static int16_t glide(int16_t cur, int16_t goal)
{
    int16_t d = goal - cur;
    if (d >= -1 && d <= 1) {
        return goal;
    }
    return cur + d / 2;
}

/* ---------------------------------------------------------------------- */
/* Public                                                                  */
/* ---------------------------------------------------------------------- */

/*
 * ui_radar_init - clear the screen and draw the dome, range labels and the
 * big range text.
 */
void ui_radar_init(void)
{
    alert_r_px = (int16_t)(((int32_t)alert_get_radius() * R_PX) / RANGE_MM);
    alert_shown = 0xFF;

    gfx_fill_screen(C_BG);
    restore_rect(APEX_X - R_PX, APEX_Y - R_PX, 2 * R_PX + 1, R_PX + 1);

    /* Range labels under the baseline, at each ring on both sides. */
    for (uint8_t i = 0; i <= N_RINGS; i++) {
        int16_t r = (i < N_RINGS) ? pgm_read_byte(&ring_px[i]) : R_PX;
        const char *lbl = (const char *)pgm_read_word(&range_lbl[i]);
        uint8_t len = 0;
        while (pgm_read_byte(&lbl[len])) {
            len++;
        }
        int16_t w = len * GFX_CHAR_W;
        int16_t xr = APEX_X + r - w / 2;
        int16_t xl = APEX_X - r - w / 2;
        if (xr + w > LCD_WIDTH) xr = LCD_WIDTH - w;
        if (xl < 0) xl = 0;
        gfx_text_P(xr, APEX_Y + 4, lbl, C_TEXT, C_BG, 1);
        gfx_text_P(xl, APEX_Y + 4, lbl, C_TEXT, C_BG, 1);
    }
    gfx_text_P(APEX_X - 2 * 6, APEX_Y + 14, PSTR("6M"), COLOR_WHITE, C_BG, 2);

    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        pings[n].on = 0;
        goals[n].want = 0;
    }
    pulse_r = 0;
    sweep_drawn = 0;
}

/*
 * ui_set_targets - take the filtered targets from track.c as new goals.
 * Called once per radar frame; drawing happens in ui_animate().
 */
void ui_set_targets(void)
{
    uint16_t radius = alert_get_radius();

    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        const track_t *t = track_get(n);
        goal_t *g = &goals[n];
        g->want = 0;
        if (!t->shown) {
            continue;
        }

        /* mm -> px at 156 px per 6000 mm; X mirrored if RADAR_X_SIGN = -1. */
        int16_t dx = (int16_t)((RADAR_X_SIGN * (int32_t)t->x * R_PX) / RANGE_MM);
        int16_t dy = (int16_t)(((int32_t)t->y * R_PX) / RANGE_MM);
        int16_t adx = (dx < 0) ? -dx : dx;
        if (dy <= 0 || (int32_t)adx * COV_DY > (int32_t)dy * COV_DX) {
            continue;                   /* outside the +/-60 degree cone */
        }

        g->px = APEX_X + dx;
        g->py = APEX_Y - dy;
        g->color = (t->dist < radius) ? C_PING_HOT : C_PING;
        g->len = format_meters(g->text, t->dist);
        g->vx = (int16_t)(RADAR_X_SIGN * t->mx);    /* screen x right */
        g->vy = (int16_t)(-t->my);                  /* screen y down  */
        g->want = 1;
    }
}

/*
 * ui_animate - one animation tick: move pings halfway to their goals,
 * erase the ones that change, then draw all visible pings (an erase can cut
 * into a neighbor).
 */
void ui_animate(void)
{
    ping_t  next[RADAR_TARGETS];
    uint8_t changed[RADAR_TARGETS];
    uint8_t any = 0;

    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        const goal_t *g = &goals[n];
        ping_t *q = &next[n];
        q->on = 0;
        if (g->want) {
            int16_t px = pings[n].on ? glide(pings[n].px, g->px) : g->px;
            int16_t py = pings[n].on ? glide(pings[n].py, g->py) : g->py;
            layout_ping(q, px, py, g);
            q->on = 1;
        }
        changed[n] = !same_ping(&pings[n], q);
        any |= changed[n];
    }
    if (!any) {
        return;
    }

    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        if (changed[n] && pings[n].on) {
            erase_ping(n);
        }
    }
    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        pings[n] = next[n];
        if (pings[n].on) {
            draw_ping(n);
        }
    }
}

#if UI_SCAN_PULSE
/*
 * redraw_pings_near_arc / _line - put back any ping the scan just crossed,
 * so pings stay on top of the animation.
 */
static void redraw_pings_near_arc(int16_t r_old, int16_t r_new)
{
    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        if (!pings[n].on) {
            continue;
        }
        box_t pb = ping_box(&pings[n]);
        if ((r_old && box_near_arc(&pb, r_old)) || box_near_arc(&pb, r_new)) {
            draw_ping(n);
        }
    }
}
#endif


/*
 * ui_scan_step - advance the scan animation by one step.
 *
 * Pulse: the radius follows the clock. If it has moved, draw the new ring,
 * then erase the old one by restoring the background under its pixels.
 * Each ring is ~(2/3)*pi*r pixels inside the cone (~320 at the rim), drawn
 * as row/column runs rather than single pixels.
 *
 * Sweep: erase/redraw a line from the apex, bouncing within the cone.
 */
void ui_scan_step(uint32_t now)
{
#if UI_SCAN_PULSE
    int16_t r = PULSE_MIN + (int16_t)(((now - pulse_t0) * PULSE_PX_PER_S) / 1000UL);
    if (pulse_r == 0 || r > R_PX - 3) {
        pulse_t0 = now;
        r = PULSE_MIN;
    }
    if (r == pulse_r) {
        return;
    }
    /* New ring first, then erase the old one, so a ring is always on screen
     * (erasing first made the outer part blink out on every step). */
    int16_t old_r = pulse_r;
    pulse_r = r;
    arc_walk(pulse_r, C_PULSE, 0);
    if (old_r) {
        arc_walk(old_r, 0, bg_color_at);
    }
    redraw_pings_near_arc(old_r, pulse_r);
#else
    (void)now;
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
    sweep_ex = APEX_X + (int16_t)(((int32_t)SWEEP_R * sin_q15(sweep_deg)) >> 15);
    sweep_ey = APEX_Y - (int16_t)(((int32_t)SWEEP_R * cos_q15(sweep_deg)) >> 15);
    gfx_line(APEX_X, APEX_Y, sweep_ex, sweep_ey, C_SWEEP);
    sweep_drawn = 1;

    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        if (!pings[n].on) {
            continue;
        }
        box_t pb = ping_box(&pings[n]);
        if (box_near_line(&pb, old_deg) || box_near_line(&pb, sweep_deg)) {
            draw_ping(n);
        }
    }
#endif
}

/*
 * ui_alert_status - bottom line: zone radius (and MUTED) on the left; on the
 * right, an 8-character field:
 *   "ARMING n" yellow, counting down the seconds left
 *   "   ARMED" green, watching
 *   "   ALERT" white on red, alarm on
 * Redrawn only when the state or the countdown second changes.
 */
void ui_alert_status(uint8_t state, uint8_t secs)
{
    uint8_t key = (state == ALERT_ARMING) ? (uint8_t)(0x10 + secs) : state;
    if (key == alert_shown) {
        return;
    }
    if (alert_shown == 0xFF) {
        char buf[LABEL_MAX + 1];
        int16_t x = gfx_text_P(4, 231, PSTR("ZONE "), C_STATUS, C_BG, 1);
        format_meters(buf, alert_get_radius());
        x = gfx_text(x, 231, buf, C_ALERT, C_BG, 1);
        if (alert_get_mute()) {
            gfx_text_P(x, 231, PSTR("  MUTED"), C_STATUS, C_BG, 1);
        }
    }
    const int16_t x_right = LCD_WIDTH - 8 * GFX_CHAR_W - 4;     /* "ARMING n" */
    if (state == ALERT_ON) {
        gfx_text_P(x_right, 231, PSTR("   ALERT"), COLOR_WHITE, COLOR_RED, 1);
    } else if (state == ALERT_ARMING) {
        int16_t x = gfx_text_P(x_right, 231, PSTR("ARMING "), COLOR_YELLOW, C_BG, 1);
        gfx_char(x, 231, (char)('0' + (secs > 9 ? 9 : secs)), COLOR_YELLOW, C_BG, 1);
    } else {
        gfx_text_P(x_right, 231, PSTR("   ARMED"), COLOR_GREEN, C_BG, 1);
    }
    alert_shown = key;
}

/*
 * ui_debug_target - small readout of target 1 in the top bar: raw X/Y from
 * the radar and the filtered X/Y, in mm.
 */
void ui_debug_target(void)
{
    const track_t *t = track_get(0);

    int16_t x = gfx_text_P(4, 14, PSTR("RAW"), C_STATUS, C_BG, 1);
    if (t->raw_ok) {
        x = gfx_int_pad(x, 14, t->raw_x, 6, COLOR_WHITE, C_BG, 1);
        gfx_int_pad(x, 14, t->raw_y, 6, COLOR_WHITE, C_BG, 1);
    } else {
        gfx_text_P(x, 14, PSTR("     -     -"), C_STATUS, C_BG, 1);
    }

    x = gfx_text_P(4, 24, PSTR("FLT"), C_STATUS, C_BG, 1);
    if (t->shown) {
        x = gfx_int_pad(x, 24, t->x, 6, C_TEXT, C_BG, 1);
        gfx_int_pad(x, 24, t->y, 6, C_TEXT, C_BG, 1);
    } else {
        gfx_text_P(x, 24, PSTR("     -     -"), C_STATUS, C_BG, 1);
    }
}

/*
 * ui_status - debug line at the top: frame count, errors, frame rate and
 * the longest animation tick (scan + glide) since the last refresh, in ms;
 * red when it overran the 30 ms tick.
 */
void ui_status(const radar_stats_t *s, uint16_t hz, uint16_t draw_ms)
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
    x = gfx_text_P(x, 2, PSTR("HZ"), C_STATUS, C_BG, 1);
    x = gfx_int_pad(x, 2, draw_ms, 4, draw_ms > 30 ? COLOR_RED : COLOR_WHITE, C_BG, 1);
    gfx_text_P(x, 2, PSTR("MS"), C_STATUS, C_BG, 1);
}
