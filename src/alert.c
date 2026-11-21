/*
 * alert.c - Alert zone logic, run once per radar frame after track_update().
 *
 * Input: the filtered, confirmed targets from track.c (already range-gated,
 * so nothing under 0.5 m counts). The nearest one decides the alert.
 *
 * Hysteresis (two thresholds) so the alarm does not chatter when someone
 * stands right at the edge and the reading wobbles by a few cm:
 *   off -> on  when nearest < R
 *   on  -> off when nearest > R + ALERT_HYST_MM, or no target is left
 * With R = 1.5 m: turns on inside 1.50 m, turns off beyond 1.75 m.
 * 250 mm is several times the filtered position noise (~50 mm std on a
 * still target, see DESIGN_NOTES), so the edge cannot flip it back and forth.
 *
 * Beep rate: linear in distance, from BEEP_FAR_MS at the zone edge down to
 * BEEP_NEAR_MS at ALERT_SOLID_MM (1.0 m):
 *   period = NEAR + (d - SOLID) * (FAR - NEAR) / (R - SOLID), clamped
 * Closer than ALERT_SOLID_MM the buzzer holds one continuous tone. While
 * the alert holds in the hysteresis band (R..R+H) it beeps at the slowest
 * rate.
 */

#include "track.h"
#include "buzzer.h"
#include "alert.h"

#define BEEP_NEAR_MS  100       /* period at ALERT_SOLID_MM  */
#define BEEP_FAR_MS   800       /* period at the zone edge   */

static uint16_t radius_mm = ALERT_RADIUS_DEFAULT_MM;
static uint8_t  muted;
static uint8_t  active;
static uint16_t nearest_mm;

void alert_set_radius(uint16_t mm)
{
    radius_mm = mm;
}

uint16_t alert_get_radius(void)
{
    return radius_mm;
}

/*
 * alert_set_mute - silence the buzzer; the zone and ALERT text still work.
 */
void alert_set_mute(uint8_t mute)
{
    muted = mute;
    if (muted) {
        buzzer_set_period(0);
    }
}

uint8_t alert_get_mute(void)
{
    return muted;
}

uint8_t alert_active(void)
{
    return active;
}

uint16_t alert_nearest_mm(void)
{
    return nearest_mm;
}

/*
 * beep_period - map distance to beep period (ms), see the header comment.
 */
static uint16_t beep_period(uint16_t d)
{
    if (d < ALERT_SOLID_MM) {
        return BUZZER_SOLID;
    }
    if (d >= radius_mm || radius_mm <= ALERT_SOLID_MM) {
        return BEEP_FAR_MS;
    }
    return BEEP_NEAR_MS +
           (uint16_t)(((uint32_t)(d - ALERT_SOLID_MM) * (BEEP_FAR_MS - BEEP_NEAR_MS))
                      / (radius_mm - ALERT_SOLID_MM));
}

/*
 * alert_update - recompute the alert from the current targets and set the
 * buzzer cadence. Returns 1 while the alert is active.
 */
uint8_t alert_update(void)
{
    nearest_mm = 0;
    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        const track_t *t = track_get(n);
        if (t->shown && (nearest_mm == 0 || t->dist < nearest_mm)) {
            nearest_mm = t->dist;
        }
    }

    if (!active) {
        active = (nearest_mm != 0 && nearest_mm < radius_mm);
    } else if (nearest_mm == 0 || nearest_mm > radius_mm + ALERT_HYST_MM) {
        active = 0;
    }

    buzzer_set_period((active && !muted) ? beep_period(nearest_mm) : 0);
    return active;
}
