/*
 * settings.h - User settings changed from the touch menu, kept in EEPROM.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>

#define UNITS_M        0
#define UNITS_FT       1

#define SCAN_PULSE     0
#define SCAN_SWEEP     1

#define THEME_RADAR    0
#define THEME_OCEAN    1
#define THEME_AMBER    2
#define THEME_CUSTOM   3
#define N_THEMES       4

#define N_PALETTE      9            /* swatches on the custom-color page */

#define RADIUS_MIN_DM  10           /* alert radius limits, 0.1 m units  */
#define RADIUS_MAX_DM  40
#define RADIUS_STEP_DM 5

typedef struct {
    uint8_t units;                  /* UNITS_M / UNITS_FT                */
    uint8_t radius_dm;              /* alert radius, 0.1 m               */
    uint8_t mute;                   /* 1 = buzzer silent                 */
    uint8_t debug;                  /* 1 = show debug lines in top bar   */
    uint8_t scan;                   /* SCAN_PULSE / SCAN_SWEEP           */
    uint8_t theme;                  /* THEME_*                           */
    uint8_t custom[3];              /* palette index: primary, secondary,
                                       tertiary (THEME_CUSTOM)           */
} settings_t;

extern settings_t settings;

void    settings_defaults(void);
void    settings_apply(void);
uint8_t settings_load(void);        /* 1 = loaded from EEPROM, 0 = defaults */
uint8_t settings_save(void);        /* 1 = something was written            */

#endif /* SETTINGS_H */
