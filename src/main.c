/*
 * main.c - mmWave presence radar firmware entry point.
 *
 * Target: ATmega328P @ 16 MHz (Arduino Uno R3), avr-gcc, no Arduino core.
 *
 * Flow:
 *   radar UART -> uart_radar.c (ring buffer + frame parser)
 *              -> track.c      (range gate, confirm/hold, alpha-beta filter)
 *              -> alert.c      (zone, hysteresis, arming) -> buzzer.c
 *              -> ui.c         (dome, scan, gliding pings)
 *   touch tap  -> menu.c       (settings, themes)  -> settings.c
 *   D2 button  -> re-arm the alarm
 *
 * Build options (Makefile): DEMO=1 synthetic targets, RADAR_SETUP=1 radar
 * baud tool, TOUCH_CAL=1 touch calibration. Earlier milestone screens are
 * in the git history.
 */

#include "board.h"
#include <avr/interrupt.h>
#include "spi.h"
#include "ili9341.h"
#include "tick.h"
#include "uart_radar.h"
#include "track.h"
#include "alert.h"
#include "buzzer.h"
#include "button.h"
#include "touch.h"
#include "settings.h"
#include "menu.h"
#include "ui.h"

/* `make RADAR_SETUP=1` builds the one-time radar baud switch instead of the
 * normal firmware. */
#ifndef RADAR_SETUP
#define RADAR_SETUP 0
#endif
#if RADAR_SETUP
#include "radar_setup.h"
#endif

/* `make TOUCH_CAL=1` builds the touch calibration program instead. */
#ifndef TOUCH_CAL
#define TOUCH_CAL 0
#endif
#if TOUCH_CAL
#include "touch_cal.h"
#endif

/* `make DEMO=1` replaces the radar with three synthetic moving targets
 * (demo.c), to check the display, filter and alert without the radar. */
#ifndef DEMO
#define DEMO 0
#endif
#if DEMO
#include "demo.h"
#endif

#define FRAME_TICK_MS   30      /* scan step + ping glide, ~33 per second    */
#define STATUS_MS       500     /* debug line refresh                        */
#define DEBUG_MS        200     /* raw vs filtered readout + arming refresh  */
#define RATE_WINDOW_MS  1000    /* frame-rate measurement window             */
#define TOUCH_MS        50      /* touch-screen poll in the radar view       */
#define NOTE_MS         3000    /* how long LOADED / DEFAULTS / SAVED shows  */

static radar_frame_t frame;     /* latest decoded frame (static: off the stack) */

/*
 * poll_radar - feed every newly completed frame through the filter.
 * Returns 1 if at least one frame arrived.
 */
static uint8_t poll_radar(void)
{
    uint8_t have_new = 0;
#if DEMO
    if (demo_frame(tick_ms(), &frame)) {
        track_update(&frame);
        have_new = 1;
    }
#else
    while (radar_poll(&frame)) {
        track_update(&frame);
        have_new = 1;
    }
#endif
    return have_new;
}

/*
 * menu_idle - runs while the menu is open: keep reading the radar so its
 * bytes don't pile up in the ring buffer. Nothing is drawn.
 */
static void menu_idle(void)
{
    poll_radar();
}

/*
 * open_menu - silence the alarm, run the menu, then save the settings to
 * EEPROM if they changed, apply them, redraw the radar view and re-arm
 * (8 s countdown), so changing settings at the device never sets the alarm
 * off. Returns 1 if the settings were saved.
 */
static uint8_t open_menu(void)
{
    alert_rearm(tick_ms());
    menu_run(menu_idle);
    uint8_t saved = settings_save();
    settings_apply();
    ui_radar_init();
    alert_rearm(tick_ms());
    ui_alert_status(alert_state(tick_ms()), alert_arming_secs(tick_ms()));
    return saved;
}

/*
 * main - init, then loop: parse radar frames, update targets and the alarm;
 * every 30 ms step the scan and glide the pings; poll touch every 50 ms
 * (a tap opens the menu); refresh debug lines on slower timers.
 *
 * Every task is short (a scan step is at most ~25 ms of SPI traffic), and
 * the 128-byte ring buffer holds ~0.4 s of radar data, so no bytes are lost
 * while drawing.
 */
int main(void)
{
    radar_stats_t stats;

    spi_init();
    ili9341_init();

#if RADAR_SETUP
    tick_init();
    sei();
    radar_setup_run();                  /* never returns */
#endif
#if TOUCH_CAL
    touch_cal_run();                    /* never returns */
#endif

    uint8_t loaded = settings_load();   /* EEPROM, or defaults if invalid */
    settings_apply();
    touch_init();
    ui_radar_init();
    ui_note(loaded ? PSTR("LOADED") : PSTR("DEFAULTS"));

    tick_init();
    radar_init();
    buzzer_init();
    button_init();
    sei();                              /* enable interrupts (tick + USART RX) */

    uint32_t last_tick   = 0;
    uint32_t last_status = 0;
    uint32_t last_debug  = 0;
    uint32_t last_touch  = 0;
    uint32_t rate_start  = 0;
    uint16_t rate_count0 = 0;
    uint16_t hz          = 0;
    uint16_t draw_max    = 0;           /* longest tick since last status (ms) */
    uint8_t  touch_was   = 1;           /* require a release before the first tap */
    uint32_t note_until  = NOTE_MS;     /* clear the bottom-center note then   */
    uint8_t  note_on     = 1;

    for (;;) {
        uint32_t now = tick_ms();

        /* Every complete frame goes through the filter (about 11 per
         * second); the filtered targets become the pings' new goals. */
        if (poll_radar()) {
            alert_update(now);
            uint8_t state = alert_state(now);
            ui_set_targets(state != ALERT_ARMING);  /* no pings while arming */
            ui_alert_status(state, alert_arming_secs(now));
        }

        /* D2 button: re-arm. Silences the alarm and restarts the quiet
         * ARMING countdown, for walking away after mounting the device. */
        if (button_pressed()) {
            alert_rearm(now);
            ui_alert_status(alert_state(now), alert_arming_secs(now));
        }

        /* Touch: a new tap anywhere on the radar view opens the menu. */
        if ((now - last_touch) >= TOUCH_MS) {
            last_touch = now;
            int16_t tx, ty;
            uint8_t down = touch_read(&tx, &ty);
            if (down && !touch_was) {
                if (open_menu()) {
                    ui_note(PSTR("SAVED"));
                    note_on = 1;
                    note_until = tick_ms() + NOTE_MS;
                }
                last_status = last_debug = 0;   /* redraw the debug lines */
                down = 1;                       /* wait for a release again */
            }
            touch_was = down;
        }

        if (note_on && (int32_t)(now - note_until) >= 0) {
            ui_note(0);
            note_on = 0;
        }

        if ((now - last_tick) >= FRAME_TICK_MS) {
            last_tick = now;
            ui_scan_step(now);
            ui_animate();
            uint16_t took = (uint16_t)(tick_ms() - now);
            if (took > draw_max) {
                draw_max = took;
            }
        }

        if ((now - last_debug) >= DEBUG_MS) {
            last_debug = now;
            if (settings.debug) {
                ui_debug_target();
            }
            /* keeps the ARMING countdown ticking between radar frames */
            ui_alert_status(alert_state(now), alert_arming_secs(now));
        }

        if ((now - last_status) >= STATUS_MS) {
            last_status = now;
            radar_get_stats(&stats);
            if ((now - rate_start) >= RATE_WINDOW_MS) {
                hz = (uint16_t)(((uint32_t)(stats.frames_ok - rate_count0) * 1000UL)
                                / (now - rate_start));
                rate_start  = now;
                rate_count0 = stats.frames_ok;
            }
            if (settings.debug) {
                ui_status(&stats, hz, draw_max);
            }
            draw_max = 0;
        }
    }
}
