/*
 * main.c - Handheld mmWave radar firmware entry point.
 *
 * Target: ATmega328P @ 16 MHz (Arduino Uno R3), avr-gcc, no Arduino core.
 *
 * Milestone 2: radar frames parsed and shown as raw numbers.
 *   - X, Y (mm), distance sqrt(x^2 + y^2) (mm) and speed (cm/s) per target
 *   - raw payload bytes of target 1, distance-gate size per target
 *   - link counters: good frames, bad tails, framing errors, overruns,
 *     ring-buffer drops, frame rate and time since the last frame
 */

#include "board.h"
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include "spi.h"
#include "ili9341.h"
#include "gfx.h"
#include "tick.h"
#include "trig.h"
#include "uart_radar.h"

/* `make RADAR_SETUP=1` builds the one-time radar baud switch instead of the
 * normal firmware. */
#ifndef RADAR_SETUP
#define RADAR_SETUP 0
#endif
#if RADAR_SETUP
#include "radar_setup.h"
#endif

/* Screen refresh periods. The display is slow (about 1.35 ms per scale-2
 * character), so values are redrawn at 5 Hz and only when they change. The
 * parser still consumes every frame as it arrives. */
#define TARGET_REFRESH_MS   200
#define STATS_REFRESH_MS    250
#define RATE_WINDOW_MS      1000
#define NO_DATA_MS          1000

/* Layout (landscape 320x240). Scale 2 cells are 12x16 px. */
#define COL_X0      10                  /* left edge of the target table */
#define CW2         12                  /* scale-2 character width       */
#define ROW_HDR     22
#define ROW_T(n)    (42 + (n) * 20)
#define ROW_RAW     108
#define ROW_GATE    120
#define ROW_STAT(n) (140 + (n) * 20)

#define FG          COLOR_WHITE
#define BG          COLOR_BLACK
#define DIM         COLOR_GRAY
#define OK_COLOR    COLOR_GREEN
#define BAD_COLOR   COLOR_RED

/* Last value drawn in each table cell, so unchanged cells are skipped.
 * INT32_MIN marks "nothing drawn yet". */
#define FIELDS 4                        /* X, Y, D, V */
static int32_t shown[RADAR_TARGETS][FIELDS];
static uint8_t shown_valid[RADAR_TARGETS];

/* Column start (in characters) and width of each table field. */
static const uint8_t field_col[FIELDS]   = { 2, 8, 14, 20 };
static const uint8_t field_width[FIELDS] = { 6, 6, 6, 5 };

/*
 * draw_static - labels that never change.
 */
static void draw_static(void)
{
    gfx_fill_screen(BG);
    int16_t tx = gfx_text_P(4, 4, PSTR("M2 LD2450 RAW  "), COLOR_YELLOW, BG, 1);
    gfx_text_P(tx, 4, PSTR("X,Y,D mm  V cm/s"), DIM, BG, 1);
    int16_t x = gfx_uint(196, 4, (uint16_t)(RADAR_BAUD / 100UL), DIM, BG, 1);
    gfx_text_P(x, 4, PSTR("00 BAUD 8N1"), DIM, BG, 1);

    /* Column headers, right-aligned over each field. */
    gfx_text_P(COL_X0, ROW_HDR, PSTR("T      X     Y     D    V"), COLOR_CYAN, BG, 2);
    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        gfx_char(COL_X0, ROW_T(n), '1' + n, COLOR_CYAN, BG, 2);
    }

    gfx_text_P(4, ROW_RAW,  PSTR("T1 RAW"), DIM, BG, 1);
    gfx_text_P(4, ROW_GATE, PSTR("GATE mm"), DIM, BG, 1);

    gfx_text_P(4,   ROW_STAT(0), PSTR("OK"),   DIM, BG, 2);
    gfx_text_P(4,   ROW_STAT(1), PSTR("BAD"),  DIM, BG, 2);
    gfx_text_P(4,   ROW_STAT(2), PSTR("FE"),   DIM, BG, 2);
    gfx_text_P(4,   ROW_STAT(3), PSTR("OVF"),  DIM, BG, 2);
    gfx_text_P(160, ROW_STAT(0), PSTR("HZ"),   DIM, BG, 2);
    gfx_text_P(160, ROW_STAT(1), PSTR("AGE"),  DIM, BG, 2);
    gfx_text_P(160, ROW_STAT(2), PSTR("DOR"),  DIM, BG, 2);

    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        shown_valid[n] = 0xFF;          /* force the first draw */
        for (uint8_t f = 0; f < FIELDS; f++) {
            shown[n][f] = INT32_MIN;
        }
    }
}

/*
 * draw_targets - update the X/Y/D/V table, gate sizes and target 1 raw bytes.
 */
static void draw_targets(const radar_frame_t *fr)
{
    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        const radar_target_t *t = &fr->t[n];
        int16_t y = ROW_T(n);

        if (!t->valid) {
            /* Empty slot: draw dashes once, then leave it alone. */
            if (shown_valid[n] != 0) {
                for (uint8_t f = 0; f < FIELDS; f++) {
                    int16_t x = COL_X0 + field_col[f] * CW2;
                    for (uint8_t k = 1; k < field_width[f]; k++) {
                        x = gfx_char(x, y, ' ', DIM, BG, 2);
                    }
                    gfx_char(x, y, '-', DIM, BG, 2);
                    shown[n][f] = INT32_MIN;
                }
                shown_valid[n] = 0;
            }
            continue;
        }

        /* Distance from the radar: sqrt(x^2 + y^2). The LD2450's 4th field is
         * the gate size, not the distance [LD2450 Table 10]. */
        int32_t xx = t->x;
        int32_t yy = t->y;
        int32_t d  = isqrt32((uint32_t)(xx * xx) + (uint32_t)(yy * yy));
        int32_t val[FIELDS] = { t->x, t->y, d, t->speed };

        for (uint8_t f = 0; f < FIELDS; f++) {
            if (val[f] != shown[n][f]) {
                gfx_int_pad(COL_X0 + field_col[f] * CW2, y, val[f],
                            field_width[f], FG, BG, 2);
                shown[n][f] = val[f];
            }
        }
        shown_valid[n] = 1;
    }

    /* Target 1 raw payload, to check the sign decoding by hand. */
    int16_t x = 52;
    for (uint8_t i = 0; i < RADAR_TARGET_BYTES; i++) {
        x = gfx_hex8(x, ROW_RAW, fr->raw[i], FG, BG, 1);
        x = gfx_char(x, ROW_RAW, ' ', FG, BG, 1);
    }

    /* Distance-gate size per target. */
    x = 52;
    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        x = gfx_int_pad(x, ROW_GATE, fr->t[n].valid ? fr->t[n].res : 0, 5, FG, BG, 1);
    }
}

/*
 * draw_stats - link counters, frame rate and data age.
 * Counters are colored red once they are non-zero.
 */
static void draw_stats(const radar_stats_t *s, uint16_t hz, uint32_t age_ms)
{
    gfx_int_pad(52, ROW_STAT(0), s->frames_ok, 6, OK_COLOR, BG, 2);
    gfx_int_pad(52, ROW_STAT(1), s->frames_bad, 6, s->frames_bad ? BAD_COLOR : FG, BG, 2);
    gfx_int_pad(52, ROW_STAT(2), s->fe, 6, s->fe ? BAD_COLOR : FG, BG, 2);
    gfx_int_pad(52, ROW_STAT(3), s->ovf, 6, s->ovf ? BAD_COLOR : FG, BG, 2);

    gfx_int_pad(208, ROW_STAT(0), hz, 3, FG, BG, 2);
    if (age_ms > NO_DATA_MS) {
        gfx_text_P(208, ROW_STAT(1), PSTR(" NONE"), BAD_COLOR, BG, 2);
    } else {
        gfx_int_pad(208, ROW_STAT(1), (int32_t)age_ms, 5, FG, BG, 2);
    }
    gfx_int_pad(208, ROW_STAT(2), s->dor, 5, s->dor ? BAD_COLOR : FG, BG, 2);
}

/*
 * main - init, then loop: parse radar bytes, refresh the screen on a timer.
 */
int main(void)
{
    static radar_frame_t frame;         /* static: keep it off the stack */
    radar_stats_t stats;

    spi_init();
    ili9341_init();

#if RADAR_SETUP
    tick_init();
    sei();
    radar_setup_run();                  /* never returns */
#endif

    draw_static();

    tick_init();
    radar_init();
    sei();                              /* enable interrupts (tick + USART RX) */

    uint8_t  have_new    = 0;
    uint32_t last_frame  = 0;
    uint32_t last_target = 0;
    uint32_t last_stats  = 0;
    uint32_t rate_start  = 0;
    uint16_t rate_count0 = 0;
    uint16_t hz          = 0;

    for (;;) {
        uint32_t now = tick_ms();

        /* Consume every complete frame; keep only the newest for display. */
        while (radar_poll(&frame)) {
            have_new   = 1;
            last_frame = now;
        }

        if (have_new && (now - last_target) >= TARGET_REFRESH_MS) {
            last_target = now;
            have_new = 0;
            draw_targets(&frame);
        }

        if ((now - last_stats) >= STATS_REFRESH_MS) {
            last_stats = now;
            radar_get_stats(&stats);

            /* Frame rate over a 1 s window (radar nominal: 10 Hz [LD2450 §2.3]). */
            if ((now - rate_start) >= RATE_WINDOW_MS) {
                hz = (uint16_t)(((uint32_t)(stats.frames_ok - rate_count0) * 1000UL)
                                / (now - rate_start));
                rate_start  = now;
                rate_count0 = stats.frames_ok;
            }
            draw_stats(&stats, hz, now - last_frame);
        }
    }
}
