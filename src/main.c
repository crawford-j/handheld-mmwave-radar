/*
 * main.c - Handheld mmWave radar firmware entry point.
 *
 * Target: ATmega328P @ 16 MHz (Arduino Uno R3), avr-gcc, no Arduino core.
 *
 * Milestone 3: radar view. Range rings, a sweep line bouncing across the
 * +/-60 degree fan, and a ping with a distance label for each target, plus
 * a debug status line with the link counters. (The milestone 2 raw-number
 * screen is in the git history.)
 */

#include "board.h"
#include <avr/interrupt.h>
#include "spi.h"
#include "ili9341.h"
#include "tick.h"
#include "uart_radar.h"
#include "ui.h"

/* `make RADAR_SETUP=1` builds the one-time radar baud switch instead of the
 * normal firmware. */
#ifndef RADAR_SETUP
#define RADAR_SETUP 0
#endif
#if RADAR_SETUP
#include "radar_setup.h"
#endif

#define SWEEP_MS        40      /* sweep step period: 2 deg / 40 ms = 50 deg/s */
#define STATUS_MS       500     /* debug line refresh                          */
#define RATE_WINDOW_MS  1000    /* frame-rate measurement window               */

/*
 * main - init, then loop: parse radar bytes, update pings on each new frame,
 * step the sweep and refresh the status line on timers.
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
    sei();                              /* enable interrupts (tick + USART RX) */

    uint32_t last_sweep  = 0;
    uint32_t last_status = 0;
    uint32_t rate_start  = 0;
    uint16_t rate_count0 = 0;
    uint16_t hz          = 0;

    for (;;) {
        uint32_t now = tick_ms();

        /* Every complete frame updates the pings (about 5 per second). */
        uint8_t have_new = 0;
        while (radar_poll(&frame)) {
            have_new = 1;
        }
        if (have_new) {
            ui_radar_targets(&frame);
        }

        if ((now - last_sweep) >= SWEEP_MS) {
            last_sweep = now;
            ui_sweep_step();
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
            ui_status(&stats, hz);
        }
    }
}
