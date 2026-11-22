/*
 * ui.h - Radar view: dome, range rings, scan animation (pulse or sweep),
 * target pings with distance labels and direction arrows, status lines.
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

/* Scan animation: 1 = expanding pulse ring from the apex (sonar style),
 * 0 = rotating sweep line (the brief's original "rotating sweep line"). */
#ifndef UI_SCAN_PULSE
#define UI_SCAN_PULSE 1
#endif

void ui_radar_init(void);
void ui_set_targets(void);
void ui_animate(void);
void ui_scan_step(uint32_t now);
void ui_status(const radar_stats_t *s, uint16_t hz, uint16_t draw_ms);
void ui_debug_target(void);
void ui_alert_status(uint8_t state, uint8_t arming_secs);   /* ALERT_* */

#endif /* UI_H */
