/*
 * menu.c - Touch settings menu.
 *
 * Two pages, both sized for the stylus (buttons >= 28 px tall). Mute is
 * not here: it is toggled by tapping the speaker icon on the radar view.
 *
 *   SETTINGS                [ RESET ] [ BACK ]
 *   UNITS   [  M  ] [ FT  ]
 *   ZONE    [  -  ]  1.5m   [  +  ]
 *   MIN     [  -  ]  0.8m   [  +  ]
 *   SCAN    [PULSE] [SWEEP]
 *   DEBUG   [ ON  ] [ OFF ]
 *   THEME   [RADAR][OCEAN][AMBER][CUSTM]
 *
 *   COLORS (custom theme)             [ BACK ]
 *   PRIMARY    nine swatches
 *   SECONDARY  nine swatches
 *   TERTIARY   nine swatches
 *
 * Input: one action per tap. A tap is a press edge; the next tap is only
 * accepted after the panel has read "not pressed" for RELEASE_READS reads
 * in a row, so one stylus touch never triggers twice.
 * Only the rows that change are redrawn.
 *
 * Settings are changed in the global `settings` struct; the caller applies
 * them and redraws the radar view when menu_run() returns.
 */

#include <avr/pgmspace.h>
#include <util/delay.h>
#include "board.h"
#include "ili9341.h"
#include "gfx.h"
#include "touch.h"
#include "settings.h"
#include "ui.h"
#include "menu.h"

/* ---- Layout ---- */
#define ROW_Y0      34          /* first row top                     */
#define ROW_H       32          /* row pitch                         */
#define BTN_H       28
#define COL_LABEL   6
#define COL_A       96          /* first button column               */
#define BTN_W       64
#define COL_B       (COL_A + BTN_W + 6)
#define RESET_X     158
#define RESET_W     74
#define BACK_X      240
#define BACK_Y      2
#define BACK_W      76
#define BACK_H      26

#define ROW_UNITS   0
#define ROW_ZONE    1
#define ROW_MIN     2
#define ROW_SCAN    3
#define ROW_DEBUG   4
#define ROW_THEME   5
#define N_ROWS      6

/* Zone and MIN rows: [-] value [+] */
#define ZONE_MINUS_X  COL_A
#define ZONE_PM_W     44
#define ZONE_VAL_X    (COL_A + ZONE_PM_W + 6)
#define ZONE_PLUS_X   (ZONE_VAL_X + 84)

/* Theme row: four narrow buttons */
#define THEME_X0    COL_A
#define THEME_W     52
#define THEME_GAP   4

/* Colors page */
#define SW_SIZE     28
#define SW_PITCH    34
#define SW_X0       8
#define SW_ROW_Y(i) (40 + (i) * 64)

#define C_BG        COLOR_BLACK
#define C_OFF       RGB565(50, 50, 50)      /* unselected button fill */
#define C_BORDER    RGB565(120, 120, 120)

#define RELEASE_READS 4

static const char s_units[]  PROGMEM = "UNITS";
static const char s_zone[]   PROGMEM = "ZONE";
static const char s_min[]    PROGMEM = "MIN";
static const char s_scan[]   PROGMEM = "SCAN";
static const char s_debug[]  PROGMEM = "DEBUG";
static const char s_theme[]  PROGMEM = "THEME";
static const char *const row_names[N_ROWS] PROGMEM = {
    s_units, s_zone, s_min, s_scan, s_debug, s_theme
};

static const char t_radar[] PROGMEM = "RADAR";
static const char t_ocean[] PROGMEM = "OCEAN";
static const char t_amber[] PROGMEM = "AMBER";
static const char t_custm[] PROGMEM = "CUSTM";
static const char *const theme_names[N_THEMES] PROGMEM = {
    t_radar, t_ocean, t_amber, t_custm
};

static uint16_t accent;             /* selected-button color: theme primary */

/* ---------------------------------------------------------------------- */

static uint8_t in_rect(int16_t x, int16_t y, int16_t rx, int16_t ry, int16_t rw, int16_t rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int16_t row_y(uint8_t row)
{
    return ROW_Y0 + row * ROW_H;
}

/*
 * pstr_len - length of a string in flash.
 */
static uint8_t pstr_len(const char *s)
{
    uint8_t n = 0;
    while (pgm_read_byte(&s[n])) {
        n++;
    }
    return n;
}

/*
 * button - filled rectangle with a 1 px border and centered text (flash
 * string). Scale 2 if it fits, else scale 1. Selected buttons use the
 * accent color with black text.
 */
static void button(int16_t x, int16_t y, int16_t w, int16_t h, const char *label, uint8_t sel)
{
    uint16_t fill = sel ? accent : C_OFF;
    uint16_t fg   = sel ? COLOR_BLACK : COLOR_WHITE;

    ili9341_fill_rect(x, y, w, h, fill);
    gfx_rect(x, y, w, h, sel ? COLOR_WHITE : C_BORDER);

    uint8_t len = pstr_len(label);
    uint8_t scale = (len * GFX_CHAR_W * 2 <= w - 4) ? 2 : 1;
    int16_t tw = len * GFX_CHAR_W * scale;
    int16_t th = GFX_CHAR_H * scale;
    gfx_text_P(x + (w - tw) / 2 + scale, y + (h - th) / 2 + scale, label, fg, fill, scale);
}

/*
 * wait_tap - wait for a new stylus press and return its screen position.
 * Keeps calling idle() so the radar is still read. Requires a clean release
 * (RELEASE_READS unpressed reads) since the previous tap.
 */
static void wait_tap(int16_t *x, int16_t *y, menu_idle_fn idle)
{
    uint8_t released = 0;
    for (;;) {
        if (idle) {
            idle();
        }
        int16_t tx, ty;
        if (touch_read(&tx, &ty)) {
            if (released >= RELEASE_READS) {
                *x = tx;
                *y = ty;
                return;
            }
            released = 0;
        } else if (released < RELEASE_READS) {
            released++;
        }
        _delay_ms(5);
    }
}

/* ---------------------------------------------------------------------- */
/* Settings page                                                           */
/* ---------------------------------------------------------------------- */

/*
 * dm_text - a distance in 0.1 m in the current units, e.g. "1.5m" / "4.9ft".
 */
static void dm_text(char *buf, uint8_t dm)
{
    uint16_t mm = (uint16_t)dm * 100;
    uint16_t tenths = (settings.units == UNITS_FT)
                    ? (uint16_t)(((uint32_t)mm * 100 + 1524) / 3048)
                    : dm;
    uint8_t i = 0;
    if (tenths >= 100) {
        buf[i++] = '0' + tenths / 100;
    }
    buf[i++] = '0' + (tenths / 10) % 10;
    buf[i++] = '.';
    buf[i++] = '0' + tenths % 10;
    if (settings.units == UNITS_FT) {
        buf[i++] = 'f';
        buf[i++] = 't';
    } else {
        buf[i++] = 'm';
    }
    buf[i] = '\0';
}

/*
 * draw_row - draw one settings row (label and its buttons).
 */
static void draw_row(uint8_t row)
{
    int16_t y = row_y(row);
    ili9341_fill_rect(0, y, LCD_WIDTH, BTN_H, C_BG);
    gfx_text_P(COL_LABEL, y + 6, (const char *)pgm_read_word(&row_names[row]),
               COLOR_WHITE, C_BG, 2);

    switch (row) {
    case ROW_UNITS:
        button(COL_A, y, BTN_W, BTN_H, PSTR("M"),  settings.units == UNITS_M);
        button(COL_B, y, BTN_W, BTN_H, PSTR("FT"), settings.units == UNITS_FT);
        break;
    case ROW_ZONE:
    case ROW_MIN: {
        char buf[8];
        button(ZONE_MINUS_X, y, ZONE_PM_W, BTN_H, PSTR("-"), 0);
        button(ZONE_PLUS_X,  y, ZONE_PM_W, BTN_H, PSTR("+"), 0);
        dm_text(buf, (row == ROW_ZONE) ? settings.radius_dm : settings.min_dm);
        gfx_text(ZONE_VAL_X + 6, y + 6, buf,
                 (row == ROW_ZONE) ? COLOR_YELLOW : COLOR_WHITE, C_BG, 2);
        break;
    }
    case ROW_SCAN:
        button(COL_A, y, BTN_W, BTN_H, PSTR("PULSE"), settings.scan == SCAN_PULSE);
        button(COL_B, y, BTN_W, BTN_H, PSTR("SWEEP"), settings.scan == SCAN_SWEEP);
        break;
    case ROW_DEBUG:
        button(COL_A, y, BTN_W, BTN_H, PSTR("ON"),  settings.debug);
        button(COL_B, y, BTN_W, BTN_H, PSTR("OFF"), !settings.debug);
        break;
    case ROW_THEME:
        for (uint8_t t = 0; t < N_THEMES; t++) {
            button(THEME_X0 + t * (THEME_W + THEME_GAP), y, THEME_W, BTN_H,
                   (const char *)pgm_read_word(&theme_names[t]), settings.theme == t);
        }
        break;
    }
}

/*
 * draw_settings - the whole settings page.
 */
static void draw_settings(void)
{
    accent = ui_theme_base(0);
    gfx_fill_screen(C_BG);
    gfx_text_P(6, 8, PSTR("SETTINGS"), accent, C_BG, 2);
    button(RESET_X, BACK_Y, RESET_W, BACK_H, PSTR("RESET"), 0);
    button(BACK_X, BACK_Y, BACK_W, BACK_H, PSTR("BACK"), 0);
    for (uint8_t r = 0; r < N_ROWS; r++) {
        draw_row(r);
    }
    gfx_text_P(COL_A, 228, PSTR("CUSTM OPENS THE COLOR PICKER"), COLOR_GRAY, C_BG, 1);
}

/* ---------------------------------------------------------------------- */
/* Colors page                                                             */
/* ---------------------------------------------------------------------- */

static const char c_pri[] PROGMEM = "PRIMARY  (DOME, RINGS)";
static const char c_sec[] PROGMEM = "SECONDARY  (PINGS, SCAN)";
static const char c_ter[] PROGMEM = "TERTIARY  (TEXT)";
static const char *const color_names[3] PROGMEM = { c_pri, c_sec, c_ter };

/*
 * draw_swatches - one row of palette squares; the chosen one gets a white
 * double outline.
 */
static void draw_swatches(uint8_t role)
{
    int16_t y = SW_ROW_Y(role) + 14;
    ili9341_fill_rect(0, y - 3, LCD_WIDTH, SW_SIZE + 6, C_BG);
    for (uint8_t i = 0; i < N_PALETTE; i++) {
        int16_t x = SW_X0 + i * SW_PITCH;
        ili9341_fill_rect(x, y, SW_SIZE, SW_SIZE, pgm_read_word(&ui_palette[i]));
        if (settings.custom[role] == i) {
            gfx_rect(x - 2, y - 2, SW_SIZE + 4, SW_SIZE + 4, COLOR_WHITE);
            gfx_rect(x - 3, y - 3, SW_SIZE + 6, SW_SIZE + 6, COLOR_WHITE);
        }
    }
}

/*
 * colors_page - pick the custom theme's three colors. Returns on BACK.
 */
static void colors_page(menu_idle_fn idle)
{
    gfx_fill_screen(C_BG);
    gfx_text_P(6, 8, PSTR("COLORS"), COLOR_WHITE, C_BG, 2);
    button(BACK_X, BACK_Y, BACK_W, BACK_H, PSTR("BACK"), 0);
    for (uint8_t role = 0; role < 3; role++) {
        gfx_text_P(SW_X0, SW_ROW_Y(role),
                   (const char *)pgm_read_word(&color_names[role]), COLOR_GRAY, C_BG, 1);
        draw_swatches(role);
    }

    for (;;) {
        int16_t x, y;
        wait_tap(&x, &y, idle);
        if (in_rect(x, y, BACK_X, BACK_Y, BACK_W, BACK_H)) {
            return;
        }
        for (uint8_t role = 0; role < 3; role++) {
            int16_t sy = SW_ROW_Y(role) + 14;
            if (y < sy - 4 || y >= sy + SW_SIZE + 4) {
                continue;
            }
            int16_t i = (x - SW_X0 + (SW_PITCH - SW_SIZE) / 2) / SW_PITCH;
            if (i >= 0 && i < N_PALETTE) {
                settings.custom[role] = (uint8_t)i;
                draw_swatches(role);
            }
        }
    }
}

/* ---------------------------------------------------------------------- */

/*
 * menu_run - show the settings page and handle taps until BACK.
 */
void menu_run(menu_idle_fn idle)
{
    draw_settings();

    for (;;) {
        int16_t x, y;
        wait_tap(&x, &y, idle);

        if (in_rect(x, y, BACK_X, BACK_Y, BACK_W, BACK_H)) {
            return;
        }
        if (in_rect(x, y, RESET_X, BACK_Y, RESET_W, BACK_H)) {
            settings_defaults();        /* factory settings; saved on BACK */
            draw_settings();
            continue;
        }
        if (y < ROW_Y0) {
            continue;
        }
        uint8_t row = (uint8_t)((y - ROW_Y0) / ROW_H);
        if (row >= N_ROWS || (y - ROW_Y0) % ROW_H >= BTN_H) {
            continue;                   /* gap between rows */
        }
        uint8_t col_a = (x >= COL_A && x < COL_A + BTN_W);
        uint8_t col_b = (x >= COL_B && x < COL_B + BTN_W);

        switch (row) {
        case ROW_UNITS:
            if (col_a || col_b) {
                settings.units = col_b ? UNITS_FT : UNITS_M;
                draw_row(ROW_UNITS);
                draw_row(ROW_ZONE);     /* zone and MIN text change units too */
                draw_row(ROW_MIN);
            }
            break;
        case ROW_ZONE:
            if (x >= ZONE_MINUS_X && x < ZONE_MINUS_X + ZONE_PM_W &&
                settings.radius_dm > RADIUS_MIN_DM) {
                settings.radius_dm -= RADIUS_STEP_DM;
                draw_row(ROW_ZONE);
                /* MIN must stay MINR_GAP_DM inside the zone: pull it in. */
                if (settings.min_dm + MINR_GAP_DM > settings.radius_dm) {
                    settings.min_dm = settings.radius_dm - MINR_GAP_DM;
                    draw_row(ROW_MIN);
                }
            } else if (x >= ZONE_PLUS_X && x < ZONE_PLUS_X + ZONE_PM_W &&
                       settings.radius_dm < RADIUS_MAX_DM) {
                settings.radius_dm += RADIUS_STEP_DM;
                draw_row(ROW_ZONE);
            }
            break;
        case ROW_MIN:                   /* 0.1 m steps, 0.5-1.5 m */
            if (x >= ZONE_MINUS_X && x < ZONE_MINUS_X + ZONE_PM_W &&
                settings.min_dm > MINR_MIN_DM) {
                settings.min_dm--;
                draw_row(ROW_MIN);
            } else if (x >= ZONE_PLUS_X && x < ZONE_PLUS_X + ZONE_PM_W &&
                       settings.min_dm < MINR_MAX_DM &&
                       settings.min_dm + 1 + MINR_GAP_DM <= settings.radius_dm) {
                settings.min_dm++;
                draw_row(ROW_MIN);
            }
            break;
        case ROW_SCAN:
            if (col_a || col_b) {
                settings.scan = col_b ? SCAN_SWEEP : SCAN_PULSE;
                draw_row(ROW_SCAN);
            }
            break;
        case ROW_DEBUG:
            if (col_a || col_b) {
                settings.debug = col_a;
                draw_row(ROW_DEBUG);
            }
            break;
        case ROW_THEME: {
            int16_t t = (x - THEME_X0) / (THEME_W + THEME_GAP);
            if (x >= THEME_X0 && t >= 0 && t < N_THEMES) {
                settings.theme = (uint8_t)t;
                if (t == THEME_CUSTOM) {
                    colors_page(idle);
                }
                draw_settings();        /* accent color follows the theme */
            }
            break;
        }
        }
    }
}
