/*
 * buzzer.c - Passive buzzer driven by Timer2 on OC2B (PD3, Uno D3, 220 ohm
 * series resistor).
 *
 * A passive buzzer needs an AC signal at its audio frequency, so Timer2
 * generates a square wave on OC2B in hardware; the CPU only turns the
 * output on and off for each beep.
 *
 * Tone: CTC mode (WGM22:0 = 010, TOP = OCR2A) with OC2B toggled on each
 * compare match (COM2B1:0 = 01) [M328P §18.7.2, Tables 18-5, 18-8].
 * The pin toggles once per timer period, so a full square-wave cycle takes
 * two periods:
 *   f = F_CPU / (2 * N * (1 + OCR2A))                    [M328P §18.7.2]
 *   N = 32 (CS22:0 = 011, Table 18-9)
 *   OCR2A = 16 MHz / (2 * 32 * 2000 Hz) - 1 = 124  ->  exactly 2000 Hz
 * OCR2B must be <= OCR2A for the match to happen; it is set equal.
 *
 * Beep cadence: buzzer_set_period(p) asks for a BEEP_ON_MS beep every p ms
 * (0 = silent, BUZZER_SOLID = continuous tone). buzzer_tick() runs inside the 1 ms Timer0 interrupt and
 * switches the tone on and off, so beeps keep exact timing even while the
 * main loop is busy drawing (a pulse step can take ~50 ms).
 */

#include "board.h"
#include <util/atomic.h>
#include "buzzer.h"

#define BUZZER_DDR    DDRD
#define BUZZER_PORT   PORTD
#define BUZZER_PIN    PD3            /* OC2B */

#define BEEP_ON_MS    40             /* length of each beep */

#define OCR_TONE      (F_CPU / (2UL * 32UL * BUZZER_HZ) - 1UL)
#if (OCR_TONE < 1) || (OCR_TONE > 255)
#error "BUZZER_HZ out of range for Timer2 with /32 prescaler"
#endif

/* Shared with the Timer0 ISR, so volatile; the 16-bit period is written
 * with interrupts off. */
static volatile uint16_t period_ms;  /* 0 = silent                       */
static volatile uint8_t  restart;    /* start a new cycle on the next tick */
static uint16_t phase_ms;            /* ms into the current cycle (ISR only) */
static volatile uint8_t  tone_on;

/*
 * tone_start - connect OC2B to the timer so the square wave reaches the pin.
 */
static void tone_start(void)
{
    TCNT2  = 0;
    /* COM2B1:0 = 01: toggle OC2B on compare match; WGM21:0 = 10: CTC. */
    TCCR2A = (1 << COM2B0) | (1 << WGM21);
    tone_on = 1;
}

/*
 * tone_stop - disconnect OC2B and hold the pin low, so no DC sits across
 * the buzzer between beeps.
 */
static void tone_stop(void)
{
    TCCR2A = (1 << WGM21);           /* COM2B1:0 = 00: OC2B disconnected */
    BUZZER_PORT &= ~(1 << BUZZER_PIN);
    tone_on = 0;
}

/*
 * buzzer_init - configure Timer2 for the tone, output silent.
 */
void buzzer_init(void)
{
    BUZZER_PORT &= ~(1 << BUZZER_PIN);
    BUZZER_DDR  |=  (1 << BUZZER_PIN);   /* OC2B only drives the pin if it is an output */

    OCR2A  = (uint8_t)OCR_TONE;          /* TOP: sets the frequency            */
    OCR2B  = (uint8_t)OCR_TONE;          /* match point for OC2B, <= OCR2A     */
    TCCR2A = (1 << WGM21);               /* CTC, OC2B disconnected for now     */
    /* WGM22 = 0; CS22:0 = 011 -> clk/32 [M328P Table 18-9]. Timer runs
     * continuously; only the pin connection is switched. */
    TCCR2B = (1 << CS21) | (1 << CS20);
    TIMSK2 = 0;                          /* no Timer2 interrupts needed        */

    period_ms = 0;
    restart = 0;
    tone_on = 0;
}

/*
 * buzzer_set_period - beep once every `p` ms; 0 stops beeping;
 * BUZZER_SOLID holds the tone on. A change takes effect at the next cycle,
 * except that starting from silent, or leaving the solid tone, starts a
 * fresh cycle (beep right away).
 */
void buzzer_set_period(uint16_t p)
{
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        if (p != 0 && (period_ms == 0 || period_ms == BUZZER_SOLID)) {
            restart = 1;                 /* beep right away on the next tick */
        }
        period_ms = p;
        if (p == 0 && tone_on) {
            tone_stop();
        }
    }
}

/*
 * buzzer_tick - run the beep cadence. Called every 1 ms from the Timer0
 * compare interrupt (tick.c), so it must stay short.
 *   phase 0 .. BEEP_ON_MS-1 : tone on
 *   rest of the period      : tone off
 */
void buzzer_tick(void)
{
    if (period_ms == 0) {
        return;
    }
    if (period_ms == BUZZER_SOLID) {     /* continuous tone */
        if (!tone_on) {
            tone_start();
        }
        return;
    }
    if (restart || ++phase_ms >= period_ms) {
        restart = 0;
        phase_ms = 0;
        tone_start();
    } else if (tone_on && phase_ms >= BEEP_ON_MS) {
        tone_stop();
    }
}
