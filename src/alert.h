/*
 * alert.h - Alert zone: beep when a target is inside a set radius, faster
 * as it gets closer, with hysteresis at the edge.
 */

#ifndef ALERT_H
#define ALERT_H

#include <stdint.h>

#define ALERT_RADIUS_DEFAULT_MM  1500
#define ALERT_HYST_MM            250     /* exit only beyond radius + this */
#define ALERT_SOLID_MM           1000    /* closer than this: continuous tone */
#define ALERT_ARM_MS             5000    /* no alarm this long after power-up */
#define ALERT_REARM_MS           8000    /* no alarm this long after a re-arm  */
#define ALERT_ENTER_FRAMES       4       /* frames inside the zone to trigger */

/* alert_update() result */
#define ALERT_IDLE    0
#define ALERT_ON      1
#define ALERT_ARMING  2

void     alert_set_radius(uint16_t mm);
uint16_t alert_get_radius(void);
void     alert_set_mute(uint8_t mute);
uint8_t  alert_get_mute(void);

uint8_t  alert_update(uint32_t now_ms);   /* returns ALERT_IDLE/ON/ARMING     */
void     alert_rearm(uint32_t now_ms);    /* restart the quiet arming delay   */
uint8_t  alert_state(uint32_t now_ms);    /* ALERT_* for display, any time    */
uint8_t  alert_arming_secs(uint32_t now_ms); /* whole seconds left, 0 = armed */
uint8_t  alert_active(void);
uint16_t alert_nearest_mm(void);          /* nearest shown target, 0 if none  */

#endif /* ALERT_H */
