/*
 * main.c - Handheld mmWave radar firmware entry point.
 *
 * Target: ATmega328P @ 16 MHz (Arduino Uno R3), avr-gcc, no Arduino core.
 *
 * Milestone 5: alert zone. The nearest confirmed target inside the alert
 * radius sets off the buzzer (Timer2 tone on D3), beeping faster as it
 * gets closer, with hysteresis at the edge (alert.c, buzzer.c).
 *
 * Milestone 4: smoothing and direction arrows. Radar frames go through
 * track.c (range gate, confirm/hold, adaptive EMA, motion history); the
 * radar view glides each ping toward its filtered position and draws an
 * arrow showing which way the target is moving.
 * (Milestone 2's raw-number screen and milestone 3's unfiltered view are in
 * the git history.)
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
#include "ui.h"

/* `make RADAR_SETUP=1` builds the one-time radar baud switch instead of the
 * normal firmware. */
#ifndef RADAR_SETUP
#define RADAR_SETUP 0
#endif
#if RADAR_SETUP
#include "radar_setup.h"
#endif

/* `make DEMO=1` replaces the radar with three synthetic moving targets
 * (demo.c), to check the display, filter and alert without the radar. */
#ifndef DEMO
#define DEMO 0
#endif
#if DEMO
#include "demo.h"
#endif

#define FRAME_TICK_MS   30      /* scan step + ping glide, ~33 per second */
#define STATUS_MS       500     /* debug line refresh                          */
#define DEBUG_MS        200     /* raw vs filtered readout refresh             */
#define RATE_WINDOW_MS  1000    /* frame-rate measurement window               */

/* Brightness steps for the D2 button, as level/256 of full color:
 * 100 %, 55 %, 30 %, 15 %. The backlight itself is hard-wired on, so this
 * dims what is drawn, not the backlight. */
static const uint8_t bright_steps[] = { 255, 140, 77, 38 };
#define N_BRIGHT (sizeof(bright_steps) / sizeof(bright_steps[0]))

/*
 * main - init, then loop: parse radar bytes and filter each new frame,
 * then on a 40 ms tick step the sweep and glide the pings; refresh the
 * status line every 500 ms.
 *
 * Every task is short (a sweep step is at most ~30 ms of SPI traffic), and
 * the 128-byte ring buffer holds ~0.4 s of radar data, so no bytes are lost
 * while drawing.
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

    ui_radar_init();

    tick_init();
    radar_init();
    buzzer_init();
    button_init();
    sei();                              /* enable interrupts (tick + USART RX) */

    uint32_t last_tick   = 0;
    uint32_t last_status = 0;
    uint32_t last_debug  = 0;
    uint32_t rate_start  = 0;
    uint16_t rate_count0 = 0;
    uint16_t hz          = 0;
    uint16_t draw_max    = 0;           /* longest tick since last status (ms) */
    uint8_t  bright_idx  = 0;

    for (;;) {
        uint32_t now = tick_ms();

        /* Every complete frame goes through the filter (about 11 per second);
         * the filtered targets become the pings' new goals. */
        uint8_t have_new = 0;
#if DEMO
        if (demo_frame(now, &frame)) {
            track_update(&frame);
            have_new = 1;
        }
#else
        while (radar_poll(&frame)) {
            track_update(&frame);
            have_new = 1;
        }
#endif
        if (have_new) {
            ui_set_targets();
            ui_alert_status(alert_update());
        }


        /* D2 button: next brightness step, then redraw everything at the new
         * level (~1-2 s; radar bytes arriving meanwhile may overflow the ring
         * buffer, which only drops a few frames). */
        if (button_pressed(now)) {
            bright_idx = (uint8_t)((bright_idx + 1) % N_BRIGHT);
            ili9341_set_brightness(bright_steps[bright_idx]);
            ui_radar_init();
            ui_set_targets();
            last_status = 0;
            last_debug = 0;
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
            ui_debug_target();
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
            ui_status(&stats, hz, draw_max);
            draw_max = 0;
        }
    }
}
