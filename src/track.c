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
 *    in a row, and removed after HOLD_FRAMES missed readings in a row.
 *    This hides one-frame ghosts (reflections, slot hand-overs) and keeps a
 *    real target from flickering when the radar drops it for a frame.
 *
 * 3. Adaptive EMA smoothing, in fixed point (position x 16):
 *        s += (raw - s) >> k
 *    k = EMA_SLOW_SHIFT (alpha = 1/4) when the reading is close to the
 *    smoothed value: this averages out the radar's frame-to-frame jitter.
 *    k = EMA_FAST_SHIFT (alpha = 1/2) when it is more than EMA_FAST_MM away:
 *    the target really moved (or the device is being turned), so follow
 *    quickly instead of lagging.
 *
 * 4. Jump reset: a reading more than JUMP_MM from the smoothed position is
 *    a different person in that slot, so the filter restarts there instead
 *    of sliding across the screen.
 *
 * 5. Motion: the last HIST smoothed positions are kept; the movement from
 *    the oldest to the newest (about 0.36 s) gives the arrow direction.
 */

#include "trig.h"
#include "track.h"

/* ---- Tuning ---- */
#define CONFIRM_FRAMES  2       /* ~0.18 s at 11 Hz before a target shows   */
#define HOLD_FRAMES     3       /* ~0.27 s of misses before it disappears   */
#define EMA_SLOW_SHIFT  2       /* alpha = 1/4 when nearly still            */
#define EMA_FAST_SHIFT  1       /* alpha = 1/2 when moving                  */
#define EMA_FAST_MM     250     /* |raw - smoothed| above this -> fast      */
#define JUMP_MM         1200    /* |raw - smoothed| above this -> reset     */
#define HIST            5       /* positions kept (newest - oldest = 4 frames) */
#define ARROW_MIN_MM    150     /* less movement than this -> no arrow      */

#define FP_SHIFT        4       /* smoothed state is mm x 16 */

typedef struct {
    int32_t sx, sy;             /* smoothed position, mm << FP_SHIFT */
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

    /* 2. Miss: hold the last position for a few frames, then drop it. */
    if (!good) {
        if (s->hits && ++s->misses >= HOLD_FRAMES) {
            reset_slot(s, o);
        }
        return;
    }
    s->misses = 0;

    int16_t dx = iabs16(t->x - (int16_t)(s->sx >> FP_SHIFT));
    int16_t dy = iabs16(t->y - (int16_t)(s->sy >> FP_SHIFT));
    int16_t err = dx + dy;              /* Manhattan distance, mm: cheap and enough */

    if (s->hits == 0 || err > JUMP_MM) {
        /* 4. New target (or a different person in this slot): start here. */
        s->sx = (int32_t)t->x << FP_SHIFT;
        s->sy = (int32_t)t->y << FP_SHIFT;
        s->hist_n = 0;
        s->hist_i = 0;
        s->hits = 0;
        o->shown = 0;
    } else {
        /* 3. Adaptive EMA. */
        uint8_t k = (err > EMA_FAST_MM) ? EMA_FAST_SHIFT : EMA_SLOW_SHIFT;
        s->sx += (((int32_t)t->x << FP_SHIFT) - s->sx) >> k;
        s->sy += (((int32_t)t->y << FP_SHIFT) - s->sy) >> k;
    }
    if (s->hits < 255) {
        s->hits++;
    }

    o->x = (int16_t)(s->sx >> FP_SHIFT);
    o->y = (int16_t)(s->sy >> FP_SHIFT);
    o->dist = isqrt32((uint32_t)((int32_t)o->x * o->x) + (uint32_t)((int32_t)o->y * o->y));
    if (s->hits >= CONFIRM_FRAMES) {
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
