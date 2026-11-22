/*
 * settings.c - The settings shared by the menu, the alert logic and the
 * radar view, and their storage in EEPROM.
 *
 * EEPROM image at address 0 (12 bytes):
 *   0       magic    0xA5 - marks the block as ours; a blank EEPROM reads
 *                    0xFF, so a never-written chip fails this check
 *   1       version  SETTINGS_VERSION - bumped whenever settings_t changes,
 *                    so an old layout is not misread as the new one
 *   2..10   settings_t, byte for byte (9 bytes)
 *   11      checksum = ~(sum of bytes 0..10) mod 256 - catches a corrupted
 *                    or half-written block (e.g. power lost mid-save)
 * On load, the magic, version, checksum and every field's range must check
 * out; otherwise the defaults are used.
 *
 * Saving writes only bytes that changed (nvm_update), and only when the
 * settings differ from what was last loaded or saved. A full save is at most
 * 12 x 3.4 ms = 41 ms [M328P Table 8-1].
 */

#include <string.h>
#include "alert.h"
#include "nvm.h"
#include "settings.h"

#define EE_ADDR            0
#define EE_MAGIC           0xA5
#define SETTINGS_VERSION   1
#define EE_SIZE            (2 + sizeof(settings_t) + 1)

settings_t settings;
static settings_t stored;           /* what the EEPROM holds right now */

/*
 * settings_defaults - factory settings: meters, 1.5 m zone, sound on, debug
 * lines on, pulse scan, RADAR theme (custom colors preset to
 * green / white / cyan).
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

/*
 * checksum - one's complement of the 8-bit sum of n bytes.
 */
static uint8_t checksum(const uint8_t *p, uint8_t n)
{
    uint8_t sum = 0;
    while (n--) {
        sum += *p++;
    }
    return (uint8_t)~sum;
}

/*
 * valid - every field within its allowed range.
 */
static uint8_t valid(const settings_t *s)
{
    return s->units <= UNITS_FT &&
           s->radius_dm >= RADIUS_MIN_DM && s->radius_dm <= RADIUS_MAX_DM &&
           (s->radius_dm % RADIUS_STEP_DM) == 0 &&
           s->mute <= 1 && s->debug <= 1 &&
           s->scan <= SCAN_SWEEP &&
           s->theme < N_THEMES &&
           s->custom[0] < N_PALETTE && s->custom[1] < N_PALETTE &&
           s->custom[2] < N_PALETTE;
}

/*
 * settings_load - read the EEPROM image. Returns 1 if it was valid and the
 * settings were loaded, 0 if the defaults were used instead (blank,
 * corrupted or old-version EEPROM).
 */
uint8_t settings_load(void)
{
    uint8_t img[EE_SIZE];
    for (uint8_t i = 0; i < EE_SIZE; i++) {
        img[i] = nvm_read(EE_ADDR + i);
    }

    settings_t s;
    memcpy(&s, &img[2], sizeof s);

    if (img[0] == EE_MAGIC && img[1] == SETTINGS_VERSION &&
        img[EE_SIZE - 1] == checksum(img, EE_SIZE - 1) && valid(&s)) {
        settings = s;
        stored = s;
        return 1;
    }

    settings_defaults();
    /* Force the next save to write a fresh, valid block. */
    memset(&stored, 0xFF, sizeof stored);
    return 0;
}

/*
 * settings_save - write the settings to EEPROM if they changed since the
 * last load/save. Returns 1 if anything was written.
 */
uint8_t settings_save(void)
{
    if (memcmp(&settings, &stored, sizeof settings) == 0) {
        return 0;
    }

    uint8_t img[EE_SIZE];
    img[0] = EE_MAGIC;
    img[1] = SETTINGS_VERSION;
    memcpy(&img[2], &settings, sizeof settings);
    img[EE_SIZE - 1] = checksum(img, EE_SIZE - 1);

    for (uint8_t i = 0; i < EE_SIZE; i++) {
        nvm_update(EE_ADDR + i, img[i]);
    }
    stored = settings;
    return 1;
}
