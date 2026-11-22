/*
 * track.h - Per-target filtering: range gate, confirm/hold, adaptive EMA
 * smoothing and recent motion for direction arrows.
 */

#ifndef TRACK_H
#define TRACK_H

#include <stdint.h>
#include "uart_radar.h"

/* Range gate. The minimum is a setting (menu MIN, 0.5-1.5 m): closer
 * readings are dropped. The radar can't tell front from back, so someone
 * sitting right behind it shows up as a close target in front. */
#define MIN_RANGE_DEFAULT_MM  800
#define MAX_RANGE_MM          6000

typedef struct {
    uint8_t  shown;     /* confirmed target, should be on screen       */
    int16_t  x, y;      /* smoothed position, mm (radar coordinates)   */
    uint16_t dist;      /* smoothed distance sqrt(x^2 + y^2), mm       */
    int16_t  mx, my;    /* movement over the history window, mm; 0,0 if
                           it moved less than ARROW_MIN_MM (no arrow)   */
    uint8_t  raw_ok;    /* last frame had a reading in range (debug)   */
    int16_t  raw_x, raw_y;  /* that unfiltered reading, mm (debug)     */
} track_t;

void           track_update(const radar_frame_t *fr);
void           track_reset(void);      /* forget all targets (radar lost) */
void           track_set_min_range(uint16_t mm);
uint16_t       track_get_min_range(void);
const track_t *track_get(uint8_t n);

#endif /* TRACK_H */
