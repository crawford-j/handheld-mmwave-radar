/*
 * demo.c - Synthetic radar frames, so the UI, tracking filter, alert zone
 * and buzzer can be exercised with the radar disconnected (`make DEMO=1`).
 *
 * Produces a decoded frame every DEMO_PERIOD_MS (the real radar's ~11 Hz)
 * with three targets, in the same coordinates the LD2450 uses
 * (x = lateral mm, y = forward mm):
 *   1: walks side to side at 2.5 m        x = 1400 sin(20 deg/s * t)
 *   2: walks in and out of the alert zone   y = 1600 + 1100 sin(15 deg/s * t)
 *   3: circles slowly around (1000, 4000)   radius 600 mm, 10 deg/s
 * Each reading gets +/-DEMO_JITTER_MM of pseudo-random noise (a 16-bit
 * Galois LFSR), roughly like the real radar, so the filter has work to do.
 */

#include "trig.h"
#include "demo.h"

#define DEMO_PERIOD_MS  90
#define DEMO_JITTER_MM  40

static uint32_t last_ms;
static uint16_t lfsr = 0xACE1;

/*
 * jitter - pseudo-random value in -DEMO_JITTER_MM..+DEMO_JITTER_MM.
 * 16-bit Galois LFSR with taps 0xB400 (maximal length, period 65535).
 */
static int16_t jitter(void)
{
    uint8_t lsb = lfsr & 1;
    lfsr >>= 1;
    if (lsb) {
        lfsr ^= 0xB400;
    }
    return (int16_t)(lfsr % (2 * DEMO_JITTER_MM + 1)) - DEMO_JITTER_MM;
}

/*
 * wave - amplitude * sin(rate_dps * t), rate in degrees per second.
 */
static int16_t wave(uint32_t t_ms, uint16_t rate_dps, int16_t amplitude, uint8_t use_cos)
{
    int16_t deg = (int16_t)((t_ms * rate_dps / 1000UL) % 360UL);
    int16_t s = use_cos ? cos_q15(deg) : sin_q15(deg);
    return (int16_t)(((int32_t)amplitude * s) >> 15);
}

/*
 * set_target - fill one decoded target slot.
 */
static void set_target(radar_target_t *t, int16_t x, int16_t y)
{
    t->valid = 1;
    t->x = x + jitter();
    t->y = y + jitter();
    t->speed = 0;
    t->res = 320;
}

/*
 * demo_frame - return 1 and fill `fr` when a new fake frame is due.
 */
uint8_t demo_frame(uint32_t now_ms, radar_frame_t *fr)
{
    if ((now_ms - last_ms) < DEMO_PERIOD_MS) {
        return 0;
    }
    last_ms = now_ms;

    set_target(&fr->t[0], wave(now_ms, 20, 1400, 0), 2500);
    set_target(&fr->t[1], -600, 1600 + wave(now_ms, 15, 1100, 0));
    set_target(&fr->t[2], 1000 + wave(now_ms, 10, 600, 1),
                          4000 + wave(now_ms, 10, 600, 0));
    return 1;
}
