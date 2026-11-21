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
 * both to draw the background and to repair it after a ping, label, arrow or
 * sweep line moves. Drawing and repairing use the same function, so a repair
 * never leaves stray pixels.
 *
 * Pings glide: each target has a goal (from track.c, updated ~11x/s) and a
 * drawn position that moves halfway to the goal on every animation tick
 * (40 ms), so the dots slide instead of jumping between radar frames.
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

/* Ring label i: top-left just above its ring, right of the center line. */
#define RING_LBL_X        (APEX_X + 3)
#define RING_LBL_Y(r)     (APEX_Y - (r) - GFX_CHAR_H - 1)
#define RING_LBL_W        (4 * GFX_CHAR_W)      /* longest label, "4.5m" */

/* ---- Colors ---- */
#define C_BG        COLOR_BLACK
#define C_RING      RGB565(0, 110, 0)
#define C_OUTER     RGB565(0, 200, 0)
#define C_EDGE      RGB565(0, 110, 0)
#define C_CENTER    RGB565(0, 50, 0)
#define C_SWEEP     RGB565(80, 255, 80)
#define C_LABEL     RGB565(0, 160, 0)
#define C_STATUS    COLOR_GRAY
#define C_ALERT     RGB565(200, 0, 0)    /* alert-zone ring */

static const uint16_t ping_color[RADAR_TARGETS] = { COLOR_YELLOW, COLOR_CYAN, COLOR_MAGENTA };

#define PING_HALF   2               /* ping is a 5x5 square            */
#define LABEL_MAX   6               /* "6.0m" + spare                  */
#define ARROW_LEN   14              /* arrow length, px                */
#define ARROW_BACK  5               /* head: back from the tip, px     */
#define ARROW_WIDE  3               /* head: half-width, px            */

/* What is drawn (or should be drawn) for one target. */
typedef struct {
    uint8_t on;                     /* visible                           */
    int16_t px, py;                 /* ping center                       */
    int16_t lx, ly;                 /* label top-left                    */
    uint8_t len;                    /* label length (chars)              */
    char    text[LABEL_MAX + 1];
    uint8_t arrow;                  /* arrow visible                     */
    int16_t ax, ay;                 /* arrow tip                         */
    int16_t h1x, h1y, h2x, h2y;     /* arrow head ends                   */
} ping_t;

/* Where a target should end up, set once per radar frame. */
typedef struct {
    uint8_t want;
    int16_t px, py;                 /* goal ping center                   */
    int16_t vx, vy;                 /* motion direction, screen px (0 = none) */
    uint8_t len;
    char    text[LABEL_MAX + 1];
} goal_t;

static ping_t pings[RADAR_TARGETS];
static goal_t goals[RADAR_TARGETS];

static int16_t sweep_deg = -SWEEP_MAX;
static int8_t  sweep_dir = 1;
static int16_t sweep_ex, sweep_ey;  /* end point of the line on screen */
static uint8_t sweep_drawn;

static int16_t alert_r_px;          /* alert-zone ring radius, px (0 = none) */
static uint8_t alert_shown = 0xFF;   /* ALERT text state on screen (0xFF = unknown) */

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
 *   alert-zone ring: same test with the alert radius; checked first so it
 *     stays red where it lands on a range ring
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

    if (alert_r_px) {
        int32_t r = alert_r_px;
        if (d2 >= r * r - r && d2 <= r * r + r) {
            return C_ALERT;
        }
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

/*
 * draw_ring_label - draw the range label for ring i.
 */
static void draw_ring_label(uint8_t i)
{
    int16_t r = pgm_read_byte(&ring_px[i]);
    const char *lbl = (const char *)pgm_read_word(&ring_lbl[i]);
    draw_label(RING_LBL_X, RING_LBL_Y(r), lbl, 1, C_LABEL);
}

/* ---- Bounding boxes, to find what an erase or the sweep touched ---- */

typedef struct {
    int16_t l, t, r, b;             /* inclusive */
} box_t;

/*
 * box_add - grow box `bx` to include point (x, y).
 */
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
    box_t bx = { p->px - PING_HALF, p->py - PING_HALF, p->px + PING_HALF, p->py + PING_HALF };
    box_add(&bx, p->lx, p->ly);
    box_add(&bx, p->lx + p->len * GFX_CHAR_W - 1, p->ly + GFX_CHAR_H - 1);
    if (p->arrow) {
        box_add(&bx, p->ax, p->ay);
        box_add(&bx, p->h1x, p->h1y);
        box_add(&bx, p->h2x, p->h2y);
    }
    return bx;
}

/*
 * ring_label_box - bounding box of ring label i.
 */
static box_t ring_label_box(uint8_t i)
{
    int16_t r = pgm_read_byte(&ring_px[i]);
    box_t bx = { RING_LBL_X, RING_LBL_Y(r),
                 RING_LBL_X + RING_LBL_W - 1, RING_LBL_Y(r) + GFX_CHAR_H - 1 };
    return bx;
}

/*
 * boxes_overlap - true if two boxes share any pixel.
 */
static uint8_t boxes_overlap(const box_t *a, const box_t *b)
{
    return !(a->r < b->l || b->r < a->l || a->b < b->t || b->b < a->t);
}

/* ---- Pings ---- */

/*
 * draw_ping - draw one target's arrow, label and square (square on top).
 */
static void draw_ping(uint8_t n)
{
    const ping_t *p = &pings[n];
    uint16_t c = ping_color[n];

    if (p->arrow) {
        gfx_line(p->px, p->py, p->ax, p->ay, c);
        gfx_line(p->ax, p->ay, p->h1x, p->h1y, c);
        gfx_line(p->ax, p->ay, p->h2x, p->h2y, c);
    }
    draw_label(p->lx, p->ly, p->text, 0, c);
    ili9341_fill_rect(p->px - PING_HALF, p->py - PING_HALF,
                      2 * PING_HALF + 1, 2 * PING_HALF + 1, c);
}

/*
 * erase_ping - restore the background under a target's square, label and
 * arrow.
 */
static void erase_ping(uint8_t n)
{
    const ping_t *p = &pings[n];
    restore_rect(p->px - PING_HALF, p->py - PING_HALF, 2 * PING_HALF + 1, 2 * PING_HALF + 1);
    restore_rect(p->lx, p->ly, p->len * GFX_CHAR_W, GFX_CHAR_H);
    if (p->arrow) {
        gfx_line_fn(p->px, p->py, p->ax, p->ay, bg_color_at);
        gfx_line_fn(p->ax, p->ay, p->h1x, p->h1y, bg_color_at);
        gfx_line_fn(p->ax, p->ay, p->h2x, p->h2y, bg_color_at);
    }
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
 * layout_ping - fill in a ping's label and arrow positions for a ping
 * center (px, py), text from goal g.
 *
 * Arrow: the goal's motion vector scaled to ARROW_LEN px. The head is two
 * short lines from the tip, ARROW_BACK px back along the shaft and
 * ARROW_WIDE px to either side (perpendicular = (-dy, dx)).
 */
static void layout_ping(ping_t *p, int16_t px, int16_t py, const goal_t *g)
{
    p->px = px;
    p->py = py;
    p->len = g->len;
    for (uint8_t i = 0; i <= g->len; i++) {
        p->text[i] = g->text[i];
    }

    /* Label to the right of the ping, or to the left near the right edge. */
    int16_t w = p->len * GFX_CHAR_W;
    p->lx = px + PING_HALF + 3;
    if (p->lx + w > LCD_WIDTH) {
        p->lx = px - PING_HALF - 3 - w;
    }
    p->ly = py - GFX_CHAR_H / 2;
    if (p->ly > LCD_HEIGHT - GFX_CHAR_H) {
        p->ly = LCD_HEIGHT - GFX_CHAR_H;
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
    if (a->px != b->px || a->py != b->py || a->arrow != b->arrow) return 0;
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

/*
 * ui_radar_init - clear the screen and draw the radar background.
 * The fan's bounding box (313 x 181 px) is streamed from bg_color_at().
 */
void ui_radar_init(void)
{
    /* Alert ring radius: mm -> px at 180 px per 6000 mm. */
    alert_r_px = (int16_t)(((int32_t)alert_get_radius() * R_PX) / RANGE_MM);
    alert_shown = 0xFF;

    gfx_fill_screen(C_BG);
    restore_rect(APEX_X - EDGE_DX, APEX_Y - R_PX, 2 * EDGE_DX + 1, R_PX + 1);
    for (uint8_t i = 0; i < N_RINGS; i++) {
        draw_ring_label(i);
    }

    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        pings[n].on = 0;
        goals[n].want = 0;
    }
    sweep_drawn = 0;
}

/*
 * ui_set_targets - take the filtered targets from track.c as new goals.
 * Called once per radar frame; drawing happens in ui_animate().
 */
void ui_set_targets(void)
{
    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        const track_t *t = track_get(n);
        goal_t *g = &goals[n];
        g->want = 0;
        if (!t->shown) {
            continue;
        }

        /* mm -> px: 180 px per 6000 mm, X mirrored if RADAR_X_SIGN = -1. */
        int16_t dx = (int16_t)((RADAR_X_SIGN * (int32_t)t->x * R_PX) / RANGE_MM);
        int16_t dy = (int16_t)(((int32_t)t->y * R_PX) / RANGE_MM);
        int16_t adx = (dx < 0) ? -dx : dx;
        if (dy <= 0 || (int32_t)adx * EDGE_DY > (int32_t)dy * EDGE_DX) {
            continue;                   /* outside the fan */
        }

        g->px = APEX_X + dx;
        g->py = APEX_Y - dy;
        g->len = format_meters(g->text, t->dist);
        /* Motion in screen terms: x right, y down (radar y is away = up). */
        g->vx = (int16_t)(RADAR_X_SIGN * t->mx);
        g->vy = (int16_t)(-t->my);
        g->want = 1;
    }
}

/*
 * ui_animate - one animation tick (every 40 ms).
 *
 *   1. Work out each ping's next state: new targets appear at their goal,
 *      existing ones move halfway to it, unwanted ones turn off.
 *   2. Erase every ping whose drawing changes.
 *   3. Redraw ring labels that an erase touched.
 *   4. Draw all visible pings (an erase may have cut into an unchanged one).
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
            box_t pb = ping_box(&pings[n]);
            for (uint8_t i = 0; i < N_RINGS; i++) {
                box_t lb = ring_label_box(i);
                if (boxes_overlap(&pb, &lb)) {
                    draw_ring_label(i);
                }
            }
        }
    }

    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        pings[n] = next[n];
        if (pings[n].on) {
            draw_ping(n);
        }
    }
}

/*
 * near_line - true if box `bx` may touch the sweep line at angle `deg`.
 * Tests the box center: perpendicular distance from the line through the
 * apex at angle a is |dx*cos(a) - dy*sin(a)| (sin/cos in Q15, >> 15 for px),
 * compared against half the box diagonal (approximated by half width +
 * half height) plus 1 px.
 */
static uint8_t near_line(const box_t *bx, int16_t deg)
{
    int32_t dx = (bx->l + bx->r) / 2 - APEX_X;
    int32_t dy = APEX_Y - (bx->t + bx->b) / 2;
    int32_t perp = (dx * cos_q15(deg) - dy * sin_q15(deg)) >> 15;
    if (perp < 0) {
        perp = -perp;
    }
    return perp <= (bx->r - bx->l) / 2 + (bx->b - bx->t) / 2 + 1;
}

/*
 * ui_sweep_step - move the sweep line SWEEP_STEP degrees, bouncing between
 * -SWEEP_MAX and +SWEEP_MAX.
 *   1. erase the old line by restoring the background under each pixel
 *   2. draw the new line
 *   3. redraw any ring label or ping the old or new line passed over, so
 *      they stay on top of the sweep
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
        box_t lb = ring_label_box(i);
        if (near_line(&lb, old_deg) || near_line(&lb, sweep_deg)) {
            draw_ring_label(i);
        }
    }
    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        if (!pings[n].on) {
            continue;
        }
        box_t pb = ping_box(&pings[n]);
        if (near_line(&pb, old_deg) || near_line(&pb, sweep_deg)) {
            draw_ping(n);
        }
    }
}

/*
 * ui_debug_target - large readout of target 1 in the top bar: the radar's
 * raw X/Y (white) above the filtered X/Y (yellow), in mm, to compare the
 * radar against the filter. Scale-2 text so it can be read on a phone video.
 * Layout keeps clear of the "6m" ring label (x 163-186, y 41-48):
 *   left  column x = 4..135   "RAW X-1234" / "FLT X-1234"
 *   right column x = 196..291 "Y 5678"
 */
void ui_debug_target(void)
{
    const track_t *t = track_get(0);
    const int16_t y_raw = 12;
    const int16_t y_flt = 30;

    int16_t x = gfx_text_P(4, y_raw, PSTR("RAW X"), COLOR_CYAN, C_BG, 2);
    if (t->raw_ok) {
        gfx_int_pad(x, y_raw, t->raw_x, 6, COLOR_WHITE, C_BG, 2);
    } else {
        gfx_text_P(x, y_raw, PSTR("     -"), COLOR_GRAY, C_BG, 2);
    }
    x = gfx_text_P(196, y_raw, PSTR("Y"), COLOR_CYAN, C_BG, 2);
    if (t->raw_ok) {
        gfx_int_pad(x, y_raw, t->raw_y, 6, COLOR_WHITE, C_BG, 2);
    } else {
        gfx_text_P(x, y_raw, PSTR("     -"), COLOR_GRAY, C_BG, 2);
    }

    x = gfx_text_P(4, y_flt, PSTR("FLT X"), COLOR_CYAN, C_BG, 2);
    if (t->shown) {
        gfx_int_pad(x, y_flt, t->x, 6, COLOR_YELLOW, C_BG, 2);
    } else {
        gfx_text_P(x, y_flt, PSTR("     -"), COLOR_GRAY, C_BG, 2);
    }
    x = gfx_text_P(196, y_flt, PSTR("Y"), COLOR_CYAN, C_BG, 2);
    if (t->shown) {
        gfx_int_pad(x, y_flt, t->y, 6, COLOR_YELLOW, C_BG, 2);
    } else {
        gfx_text_P(x, y_flt, PSTR("     -"), COLOR_GRAY, C_BG, 2);
    }
}

/*
 * ui_alert_status - bottom line: zone radius and mute on the left, a red
 * "ALERT" on the right while the alert is active. Redrawn only on change.
 */
void ui_alert_status(uint8_t active)
{
    if (active == alert_shown) {
        return;
    }
    if (alert_shown == 0xFF) {          /* first call: static part */
        char buf[LABEL_MAX + 1];
        int16_t x = gfx_text_P(4, 231, PSTR("ZONE "), C_STATUS, C_BG, 1);
        format_meters(buf, alert_get_radius());
        x = gfx_text(x, 231, buf, C_ALERT, C_BG, 1);
        if (alert_get_mute()) {
            gfx_text_P(x, 231, PSTR("  MUTED"), C_STATUS, C_BG, 1);
        }
    }
    gfx_text_P(LCD_WIDTH - 5 * GFX_CHAR_W - 4, 231, active ? PSTR("ALERT") : PSTR("     "),
               COLOR_WHITE, active ? COLOR_RED : C_BG, 1);
    alert_shown = active;
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
