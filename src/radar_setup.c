/*
 * radar_setup.c - One-time LD2450 baud switch.
 *
 * The AVR cannot make the radar's factory 256000 from 16 MHz; the closest is
 * 250000 (-2.34 %). That worked with zero errors in testing, but leaves no
 * margin, so the radar is reprogrammed once to RADAR_SETUP_BAUD. The setting
 * is stored in the radar and survives power-off [LD2450 §2.2.7].
 *
 *   38400: +0.16 % on the AVR side (normal speed, UBRR 25). Used first, but
 *          the radar only sent about 6 frames/s at this rate.
 *   57600: -0.79 % (double speed, UBRR 34), inside the +/-1.5 % recommended
 *          for U2X mode [M328P Table 20-3].
 *
 * Sequence (each command waits for its ACK, up to 3 tries) [LD2450 §2.4]:
 *   1. Find the radar: listen for valid data frames at each candidate rate
 *      in turn until one works. Nothing is sent until the radar has been
 *      heard, so this is safe with the radar TX wire unplugged.
 *   2. Enable configuration  0x00FF value 0x0001          [§2.2.1]
 *   3. Set baud              0x00A1 value = index         [§2.2.7, Table 6]
 *   4. Restart               0x00A3                       [§2.2.9]
 *   5. Reopen the UART at the new rate, confirm data frames arrive and
 *      measure the frame rate over 3 s.
 *
 * Risk: if a command is garbled, the radar does not ACK it and its settings
 * are unchanged, so the step is retried. If setup fails after the baud
 * command was accepted, step 1 finds the radar at any candidate rate on the
 * next reset.
 */

#include "board.h"
#include <avr/pgmspace.h>
#include <util/delay.h>
#include "ili9341.h"
#include "gfx.h"
#include "tick.h"
#include "uart_radar.h"
#include "radar_setup.h"

/* Target rate and its index in [LD2450 Table 6]. */
#ifndef RADAR_SETUP_BAUD
#define RADAR_SETUP_BAUD 38400UL
#endif
#if RADAR_SETUP_BAUD == 38400UL
#define SETUP_BAUD_IDX 0x0003
#elif RADAR_SETUP_BAUD == 57600UL
#define SETUP_BAUD_IDX 0x0004
#else
#error "RADAR_SETUP_BAUD must be 38400 or 57600"
#endif

#define FIND_LISTEN_MS   1500   /* listen time per rate while searching   */
#define FIND_FRAMES      3      /* frames needed to accept a rate         */
#define ACK_TIMEOUT_MS   1000
#define CMD_TRIES        3
#define REBOOT_WAIT_MS   2000   /* radar boot time after the restart ACK  */
#define CHECK_MS         5000   /* time allowed to see frames at new rate */
#define RATE_MS          3000   /* window for the frame-rate measurement  */

#define LINE_H           20     /* scale-2 text, 16 px + 4 px gap         */

/* Rates the radar might currently be at: factory, then earlier setups. */
static const uint32_t candidates[] PROGMEM = { 250000UL, 38400UL, 57600UL };
#define N_CANDIDATES (sizeof(candidates) / sizeof(candidates[0]))

static int16_t line_y = 30;     /* y of the next log line */

/*
 * log_line - print a line of status text (from flash) and move down.
 * Returns the x position after the text, so a result can follow it.
 */
static int16_t log_line(const char *msg, uint16_t color)
{
    int16_t x = gfx_text_P(4, line_y, msg, color, COLOR_BLACK, 2);
    line_y += LINE_H;
    return x;
}

/*
 * log_baud - print a baud rate after the previous log line's text.
 * gfx_uint is 16-bit, so print baud/100 followed by "00" (all candidate
 * rates are multiples of 100).
 */
static int16_t log_baud(int16_t x, uint32_t baud, uint16_t color)
{
    x = gfx_uint(x, line_y - LINE_H, (uint16_t)(baud / 100UL), color, COLOR_BLACK, 2);
    return gfx_text_P(x, line_y - LINE_H, PSTR("00"), color, COLOR_BLACK, 2);
}

/*
 * log_result - print OK/FAIL at x on the previous log line.
 */
static void log_result(int16_t x, uint8_t ok)
{
    gfx_text_P(x, line_y - LINE_H, ok ? PSTR(" OK") : PSTR(" FAIL"),
               ok ? COLOR_GREEN : COLOR_RED, COLOR_BLACK, 2);
}

/*
 * count_frames - open the link at `baud` and count valid data frames for up
 * to `ms` milliseconds, stopping early once `need` frames have arrived.
 */
static uint8_t count_frames(uint32_t baud, uint16_t ms, uint8_t need)
{
    static radar_frame_t fr;
    uint8_t n = 0;

    radar_set_baud(baud);
    uint32_t start = tick_ms();
    while ((tick_ms() - start) < ms) {
        while (radar_poll(&fr)) {
            if (++n >= need) {
                return n;
            }
        }
    }
    return n;
}

/*
 * command - send a command and wait for its ACK, up to CMD_TRIES times.
 * Returns 1 if the radar answered (cmd | 0x0100) with status 0.
 */
static uint8_t command(uint16_t cmd, const uint8_t *value, uint8_t vlen)
{
    static radar_frame_t fr;
    uint16_t ack_cmd, ack_status;

    for (uint8_t attempt = 0; attempt < CMD_TRIES; attempt++) {
        while (radar_get_ack(&ack_cmd, &ack_status)) {
            /* discard stale ACKs */
        }
        radar_send_cmd(cmd, value, vlen);

        uint32_t start = tick_ms();
        while ((tick_ms() - start) < ACK_TIMEOUT_MS) {
            radar_poll(&fr);            /* data frames are drained and ignored */
            if (radar_get_ack(&ack_cmd, &ack_status) &&
                ack_cmd == (cmd | RADAR_ACK_BIT)) {
                if (ack_status == 0) {
                    return 1;
                }
                break;                  /* radar refused: try again */
            }
        }
    }
    return 0;
}

/*
 * halt - stop here with a final message. Press reset to run setup again.
 */
static void halt(const char *msg, uint16_t color) __attribute__((noreturn));
static void halt(const char *msg, uint16_t color)
{
    log_line(msg, color);
    log_line(PSTR("PRESS RESET TO RERUN"), COLOR_GRAY);
    for (;;) {
    }
}

/*
 * measure_rate - log the frame rate at `baud` over RATE_MS.
 */
static void measure_rate(uint32_t baud)
{
    uint8_t n = count_frames(baud, RATE_MS, 255);
    int16_t x = log_line(PSTR("FRAME RATE "), COLOR_WHITE);
    x = gfx_uint(x, line_y - LINE_H, (uint16_t)n * 10 / (RATE_MS / 100), COLOR_GREEN,
                 COLOR_BLACK, 2);
    gfx_text_P(x, line_y - LINE_H, PSTR(" HZ"), COLOR_GREEN, COLOR_BLACK, 2);
}

/*
 * radar_setup_run - run the baud switch and report each step on screen.
 * Expects spi/ili9341/tick initialized and interrupts enabled.
 */
void radar_setup_run(void)
{
    static const uint8_t val_enable[2] = { 0x01, 0x00 };        /* 0x0001 LE */
    static const uint8_t val_baud[2]   = { SETUP_BAUD_IDX & 0xFF,
                                           SETUP_BAUD_IDX >> 8 };

    gfx_fill_screen(COLOR_BLACK);
    int16_t x = gfx_text_P(4, 4, PSTR("RADAR BAUD SETUP -> "), COLOR_YELLOW, COLOR_BLACK, 1);
    x = gfx_uint(x, 4, (uint16_t)(RADAR_SETUP_BAUD / 100UL), COLOR_YELLOW, COLOR_BLACK, 1);
    gfx_text_P(x, 4, PSTR("00"), COLOR_YELLOW, COLOR_BLACK, 1);

    /* 1. Find the radar at any candidate rate. */
    x = log_line(PSTR("FIND @"), COLOR_WHITE);
    gfx_text_P(4, 220, PSTR("RADAR TX MUST BE IN D0"), COLOR_GRAY, COLOR_BLACK, 1);
    uint32_t found = 0;
    while (!found) {
        for (uint8_t i = 0; i < N_CANDIDATES && !found; i++) {
            uint32_t rate = pgm_read_dword(&candidates[i]);
            if (count_frames(rate, FIND_LISTEN_MS, FIND_FRAMES) >= FIND_FRAMES) {
                found = rate;
            }
        }
    }
    gfx_text_P(4, 220, PSTR("                      "), COLOR_GRAY, COLOR_BLACK, 1);
    log_baud(x, found, COLOR_GREEN);

    if (found == RADAR_SETUP_BAUD) {
        log_line(PSTR("ALREADY SET"), COLOR_GREEN);
        measure_rate(found);
        halt(PSTR("NOTHING TO DO"), COLOR_GREEN);
    }
    measure_rate(found);                /* rate before the change, for comparison */
    radar_set_baud(found);

    /* 2-4. Configure at the rate the radar is using now. */
    x = log_line(PSTR("ENABLE CONFIG"), COLOR_WHITE);
    uint8_t ok = command(RADAR_CMD_ENABLE_CFG, val_enable, sizeof(val_enable));
    log_result(x, ok);
    if (!ok) {
        halt(PSTR("NO ACK: UNCHANGED"), COLOR_RED);
    }

    x = log_line(PSTR("SET BAUD "), COLOR_WHITE);
    x = log_baud(x, RADAR_SETUP_BAUD, COLOR_WHITE);
    ok = command(RADAR_CMD_SET_BAUD, val_baud, sizeof(val_baud));
    log_result(x, ok);
    if (!ok) {
        command(RADAR_CMD_END_CFG, 0, 0);   /* leave config mode cleanly */
        halt(PSTR("NO ACK: UNCHANGED"), COLOR_RED);
    }

    /* The new rate takes effect only after a restart [LD2450 §2.2.7]. The
     * ACK may be lost as the radar reboots, so step 5 is the real check. */
    x = log_line(PSTR("RESTART"), COLOR_WHITE);
    log_result(x, command(RADAR_CMD_RESTART, 0, 0));
    _delay_ms(REBOOT_WAIT_MS);

    /* 5. Confirm at the new rate, then measure its frame rate. */
    x = log_line(PSTR("CHECK "), COLOR_WHITE);
    x = log_baud(x, RADAR_SETUP_BAUD, COLOR_WHITE);
    ok = (count_frames(RADAR_SETUP_BAUD, CHECK_MS, FIND_FRAMES) >= FIND_FRAMES);
    log_result(x, ok);
    if (!ok) {
        halt(PSTR("NO DATA AT NEW RATE"), COLOR_RED);
    }
    measure_rate(RADAR_SETUP_BAUD);

    halt(PSTR("DONE"), COLOR_GREEN);
}
