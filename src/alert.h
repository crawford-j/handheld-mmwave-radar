/*
 * alert.h - Alert zone: beep when a target is inside a set radius, faster
 * as it gets closer, with hysteresis at the edge.
 */

#ifndef ALERT_H
#define ALERT_H

#include <stdint.h>

#define ALERT_RADIUS_DEFAULT_MM  1500
#define ALERT_HYST_MM            250     /* exit only beyond radius + this */

void     alert_set_radius(uint16_t mm);
uint16_t alert_get_radius(void);
void     alert_set_mute(uint8_t mute);
uint8_t  alert_get_mute(void);

uint8_t  alert_update(void);              /* returns 1 if the alert is active */
uint8_t  alert_active(void);
uint16_t alert_nearest_mm(void);          /* nearest shown target, 0 if none  */

#endif /* ALERT_H */
