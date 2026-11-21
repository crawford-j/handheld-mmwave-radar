/*
 * button.c - Push-button on D2 (PD2 / INT0), wired to GND.
 *
 * The pin uses the AVR's internal pull-up, so it reads 1 when released and
 * 0 when pressed. Mechanical contacts bounce for a few ms, so the pin is
 * sampled every SAMPLE_MS and a new level is only accepted after it has
 * read the same STABLE_SAMPLES times in a row (20 ms). A press is reported
 * once, on the released -> pressed edge.
 *
 * Polling is used instead of INT0: the button only needs ~10 ms response,
 * and an edge interrupt would fire on every bounce anyway.
 */

#include "board.h"
#include "button.h"

#define BTN_DDR         DDRD
#define BTN_PORT        PORTD
#define BTN_PIN_REG     PIND
#define BTN_PIN         PD2

#define SAMPLE_MS       5
#define STABLE_SAMPLES  4       /* 4 x 5 ms = 20 ms stable */

static uint32_t last_sample;
static uint8_t  stable_level = 1;   /* 1 = released */
static uint8_t  same_count;

/*
 * button_init - PD2 as input with the internal pull-up enabled
 * (DDxn = 0, PORTxn = 1) [M328P §14.2.1, Table 14-1].
 */
void button_init(void)
{
    BTN_DDR  &= ~(1 << BTN_PIN);
    BTN_PORT |=  (1 << BTN_PIN);
}

/*
 * button_pressed - call often; returns 1 once per debounced press.
 */
uint8_t button_pressed(uint32_t now_ms)
{
    if ((now_ms - last_sample) < SAMPLE_MS) {
        return 0;
    }
    last_sample = now_ms;

    uint8_t level = (BTN_PIN_REG & (1 << BTN_PIN)) ? 1 : 0;
    if (level == stable_level) {
        same_count = 0;
        return 0;
    }
    if (++same_count < STABLE_SAMPLES) {
        return 0;
    }
    same_count = 0;
    stable_level = level;
    return (level == 0);                /* new stable level is "pressed" */
}
