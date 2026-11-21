/*
 * ui.h - Radar view: fan, range rings, sweep line, target pings and labels.
 */

#ifndef UI_H
#define UI_H

#include <stdint.h>
#include "uart_radar.h"

/* Set to -1 if a target walking to your right moves left on the screen
 * (depends on which way the radar faces relative to the display). */
#ifndef RADAR_X_SIGN
#define RADAR_X_SIGN 1
#endif

/* Targets closer than this are ignored: that is the person holding it. */
#define MIN_RANGE_MM  500

void ui_radar_init(void);
void ui_radar_targets(const radar_frame_t *fr);
void ui_sweep_step(void);
void ui_status(const radar_stats_t *s, uint16_t hz);

#endif /* UI_H */
