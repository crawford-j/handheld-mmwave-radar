/*
 * track.c - Target filtering between the radar parser and the display.
 *
 * Runs once per radar frame (about 11 per second). For each of the radar's
 * three target slots:
 *
 * 1. Range gate: a reading counts only if the slot is valid and the target
 *    is MIN_RANGE_MM..MAX_RANGE_MM away. Under 0.5 m is the person holding
 *    the device; 6 m is the radar's maximum range [LD2450 manual §2.1].
 *
 * 2. Confirm / hold: a target is shown after CONFIRM_FRAMES good readings
 *    in a row (CONFIRM_NEAR_FRAMES when closer than NEAR_MM, where
 *    reflections off the person holding or setting up the device show up),
 *    and removed after HOLD_FRAMES missed readings in a row.
 *    This hides one-frame ghosts (reflections, slot hand-overs) and keeps a
 *    real target from flickering when the radar drops it for a frame.
 *
 * 3. Adaptive alpha-beta filter, in fixed point (mm x 16). Each slot keeps
 *    a position s and a velocity v (per frame). Every frame:
 *        pred = s + v                  predict where the target should be
 *        r    = raw - pred             residual: how wrong the prediction was
 *        s    = pred + (r >> ka)       correct the position by alpha = 2^-ka
 *        v    = v    + (r >> kb)       correct the velocity by beta  = 2^-kb
 *    Because it predicts with the velocity, a target moving at a steady
 *    speed (a walk, or the device being turned slowly) is followed with no
 *    steady lag. A plain average (the first version) always trailed it.
 *    Gains: alpha = 1/4, beta = 1/16 while the residual is small (smooth
 *    out jitter); alpha = 1/2, beta = 1/8 when it is above AB_FAST_MM (the
 *    motion changed, catch up quickly). Both pairs are stable
 *    (0 < alpha < 1, 0 < beta, 4 - 2*alpha - beta > 0).
 *
 * 4. Jump reset: a reading more than JUMP_MM from the prediction is a
 *    different person in that slot, so the filter restarts there instead of
 *    sliding across the screen.
 *
 * 5. Motion: the last HIST smoothed positions are kept; the movement from
 *    the oldest to the newest (about 0.36 s) gives the arrow direction.
 */

#include "trig.h"
#include "track.h"

/* ---- Tuning ---- */
#define CONFIRM_FRAMES  2       /* ~0.18 s at 11 Hz before a target shows   */
#define CONFIRM_NEAR_FRAMES 5   /* ~0.45 s for targets closer than NEAR_MM  */
#define NEAR_MM         1000
#define HOLD_FRAMES     3       /* ~0.27 s of misses before it disappears   */
#define AB_SLOW_A       2       /* alpha = 1/4 while the prediction is good */
#define AB_SLOW_B       4       /* beta  = 1/16                             */
#define AB_FAST_A       1       /* alpha = 1/2 when the prediction is off   */
#define AB_FAST_B       3       /* beta  = 1/8                              */
#define AB_FAST_MM      250     /* |raw - pred| above this -> fast gains    */
#define V_MAX_MM        300     /* velocity clamp, mm/frame (~3.3 m/s)      */
#define JUMP_MM         1200    /* |raw - pred| above this -> reset         */
#define HIST            5       /* positions kept (newest - oldest = 4 frames) */
#define ARROW_MIN_MM    150     /* less movement than this -> no arrow      */

#define FP_SHIFT        4       /* smoothed state is mm x 16 */

typedef struct {
    int32_t sx, sy;             /* smoothed position, mm << FP_SHIFT */
    int32_t vx, vy;             /* velocity, (mm << FP_SHIFT) per frame */
    uint8_t hits;               /* consecutive good readings          */
    uint8_t misses;             /* consecutive missed readings        */
    uint8_t hist_n;             /* entries in hist (up to HIST)       */
    uint8_t hist_i;             /* next write index                   */
    int16_t hist_x[HIST], hist_y[HIST];
} slot_t;

static slot_t  slots[RADAR_TARGETS];
static track_t out[RADAR_TARGETS];

/*
 * iabs16 - absolute value for int16 differences.
 */
static int16_t iabs16(int16_t v)
{
    return (v < 0) ? (int16_t)-v : v;
}

/*
 * reset_slot - forget a slot's filter state and history.
 */
static void reset_slot(slot_t *s, track_t *o)
{
    s->hits = 0;
    s->misses = 0;
    s->hist_n = 0;
    s->hist_i = 0;
    o->shown = 0;
    o->mx = o->my = 0;
}

/*
 * update_slot - apply one frame's reading (or a miss) to slot n.
 */
static void update_slot(uint8_t n, const radar_target_t *t)
{
    slot_t  *s = &slots[n];
    track_t *o = &out[n];

    /* 1. Range gate. */
    uint8_t good = 0;
    if (t->valid && t->y > 0) {
        int32_t x = t->x;
        int32_t y = t->y;
        uint16_t d = isqrt32((uint32_t)(x * x) + (uint32_t)(y * y));
        good = (d >= MIN_RANGE_MM && d <= MAX_RANGE_MM);
    }

    /* Keep the unfiltered reading for the debug readout. */
    o->raw_ok = good;
    if (good) {
        o->raw_x = t->x;
        o->raw_y = t->y;
    }

    /* 2. Miss: hold the last position for a few frames, then drop it.
     * The velocity is zeroed so the target does not lurch when it returns. */
    if (!good) {
        s->vx = s->vy = 0;
        if (s->hits && ++s->misses >= HOLD_FRAMES) {
            reset_slot(s, o);
        }
        return;
    }
    s->misses = 0;

    int32_t px = s->sx + s->vx;         /* prediction */
    int32_t py = s->sy + s->vy;
    int32_t rx = ((int32_t)t->x << FP_SHIFT) - px;     /* residual */
    int32_t ry = ((int32_t)t->y << FP_SHIFT) - py;
    int16_t err = iabs16((int16_t)(rx >> FP_SHIFT)) + iabs16((int16_t)(ry >> FP_SHIFT));
                                        /* Manhattan distance, mm: cheap and enough */

    if (s->hits == 0 || err > JUMP_MM) {
        /* 4. New target (or a different person in this slot): start here. */
        s->sx = (int32_t)t->x << FP_SHIFT;
        s->sy = (int32_t)t->y << FP_SHIFT;
        s->vx = s->vy = 0;
        s->hist_n = 0;
        s->hist_i = 0;
        s->hits = 0;
        o->shown = 0;
    } else {
        /* 3. Adaptive alpha-beta update. */
        uint8_t fast = (err > AB_FAST_MM);
        uint8_t ka = fast ? AB_FAST_A : AB_SLOW_A;
        uint8_t kb = fast ? AB_FAST_B : AB_SLOW_B;
        s->sx = px + (rx >> ka);
        s->sy = py + (ry >> ka);
        s->vx += rx >> kb;
        s->vy += ry >> kb;

        /* Clamp the velocity so one bad reading cannot fling the target. */
        const int32_t vmax = (int32_t)V_MAX_MM << FP_SHIFT;
        if (s->vx >  vmax) s->vx =  vmax;
        if (s->vx < -vmax) s->vx = -vmax;
        if (s->vy >  vmax) s->vy =  vmax;
        if (s->vy < -vmax) s->vy = -vmax;
    }
    if (s->hits < 255) {
        s->hits++;
    }

    o->x = (int16_t)(s->sx >> FP_SHIFT);
    o->y = (int16_t)(s->sy >> FP_SHIFT);
    o->dist = isqrt32((uint32_t)((int32_t)o->x * o->x) + (uint32_t)((int32_t)o->y * o->y));
    if (s->hits >= ((o->dist < NEAR_MM) ? CONFIRM_NEAR_FRAMES : CONFIRM_FRAMES)) {
        o->shown = 1;
    }

    /* 5. Motion history (ring buffer of smoothed positions). */
    s->hist_x[s->hist_i] = o->x;
    s->hist_y[s->hist_i] = o->y;
    s->hist_i = (s->hist_i + 1) % HIST;
    if (s->hist_n < HIST) {
        s->hist_n++;
    }

    o->mx = o->my = 0;
    if (s->hist_n == HIST) {
        /* hist_i now points at the oldest entry. */
        int16_t mx = o->x - s->hist_x[s->hist_i];
        int16_t my = o->y - s->hist_y[s->hist_i];
        if (iabs16(mx) + iabs16(my) >= ARROW_MIN_MM) {
            o->mx = mx;
            o->my = my;
        }
    }
}

/*
 * track_update - process one radar frame.
 */
void track_update(const radar_frame_t *fr)
{
    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        update_slot(n, &fr->t[n]);
    }
}

/*
 * track_get - filtered state of slot n (0..2).
 */
const track_t *track_get(uint8_t n)
{
    return &out[n];
}
