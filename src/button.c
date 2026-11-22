/*
 * button.c - Push-button on D2 (PD2 / INT0), wired to GND.
 *
 * The pin uses the AVR's internal pull-up, so it reads 1 when released and
 * 0 when pressed. Mechanical contacts bounce for a few ms, so a new level
 * is only accepted after the pin has read the same for STABLE_MS
 * consecutive samples.
 *
 * Sampling runs in the 1 ms Timer0 interrupt (button_tick, called from
 * tick.c), not the main loop. The main loop can be busy drawing for 20+ ms
 * at a time; polling from there let quick taps slip between samples.
 *
 * Two kinds of press:
 *   short - released before LONG_MS; reported on release (re-arm / wake)
 *   long  - held for LONG_MS; reported as soon as the hold reaches LONG_MS,
 *           so the user gets feedback without letting go (standby). The
 *           release that follows is not reported as a short press.
 * Each event increments a counter that the main loop drains with
 * button_event(), so no press is lost even if several arrive while the
 * loop is busy.
 *
 * Polling (in the ISR) is used instead of the INT0 edge interrupt, because
 * INT0 would fire on every bounce edge and still need debouncing.
 */

#include "board.h"
#include <util/atomic.h>
#include "button.h"

#define BTN_DDR         DDRD
#define BTN_PORT        PORTD
#define BTN_PIN_REG     PIND
#define BTN_PIN         PD2

#define STABLE_MS       8       /* level must hold 8 ms to count        */
#define LONG_MS         1500    /* hold this long for a long press      */

static uint8_t  stable_level = 1;       /* 1 = released (ISR only)       */
static uint8_t  same_count;             /* ISR only                      */
static uint16_t held_ms;                /* time held so far (ISR only)   */
static uint8_t  long_sent;              /* long press already reported   */
static volatile uint8_t shorts;         /* pending short presses         */
static volatile uint8_t longs;          /* pending long presses          */

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
 * button_tick - sample and debounce the pin, time the hold. Called every
 * 1 ms from the Timer0 compare interrupt, so it must stay short.
 */
void button_tick(void)
{
    uint8_t level = (BTN_PIN_REG & (1 << BTN_PIN)) ? 1 : 0;

    if (stable_level == 0 && !long_sent && ++held_ms >= LONG_MS) {
        long_sent = 1;
        if (longs < 255) {
            longs++;
        }
    }

    if (level == stable_level) {
        same_count = 0;
        return;
    }
    if (++same_count < STABLE_MS) {
        return;
    }
    same_count = 0;
    stable_level = level;
    if (level == 0) {                   /* released -> pressed */
        held_ms = 0;
        long_sent = 0;
    } else if (!long_sent && shorts < 255) {    /* pressed -> released */
        shorts++;
    }
}

/*
 * button_event - return the next pending press (BTN_LONG first, then
 * BTN_SHORT), or BTN_NONE. The counters are shared with the ISR, so they
 * are read-modify-written with interrupts off.
 */
uint8_t button_event(void)
{
    uint8_t ev = BTN_NONE;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        if (longs) {
            longs--;
            ev = BTN_LONG;
        } else if (shorts) {
            shorts--;
            ev = BTN_SHORT;
        }
    }
    return ev;
}
