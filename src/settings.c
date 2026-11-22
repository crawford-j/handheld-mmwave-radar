/*
 * settings.c - The settings shared by the menu, the alert logic and the
 * radar view. One struct, so milestone 7 can save and load it to EEPROM as
 * a single block.
 */

#include "alert.h"
#include "settings.h"

settings_t settings;

/*
 * settings_defaults - factory settings: meters, 1.5 m zone, sound on, debug
 * lines on, pulse scan, RADAR theme (custom colors preset to
 * green / white / light green).
 */
void settings_defaults(void)
{
    settings.units     = UNITS_M;
    settings.radius_dm = ALERT_RADIUS_DEFAULT_MM / 100;
    settings.mute      = 0;
    settings.debug     = 1;
    settings.scan      = SCAN_PULSE;
    settings.theme     = THEME_RADAR;
    settings.custom[0] = 0;         /* green  */
    settings.custom[1] = 8;         /* white  */
    settings.custom[2] = 1;         /* cyan   */
}

/*
 * settings_apply - push the settings that live in other modules (alert
 * radius, mute). The radar view reads the rest directly when it redraws.
 */
void settings_apply(void)
{
    alert_set_radius((uint16_t)settings.radius_dm * 100);
    alert_set_mute(settings.mute);
}
