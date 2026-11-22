/*
 * track.h - Per-target filtering: range gate, confirm/hold, adaptive EMA
 * smoothing and recent motion for direction arrows.
 */

#ifndef TRACK_H
#define TRACK_H

#include <stdint.h>
#include "uart_radar.h"

/* Targets closer than this are ignored: that is the person holding it. */
#define MIN_RANGE_MM   500
#define MAX_RANGE_MM   6000

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
const track_t *track_get(uint8_t n);

#endif /* TRACK_H */
