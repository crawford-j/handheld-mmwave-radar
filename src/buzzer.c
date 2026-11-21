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
 * (0 = silent). buzzer_service() is called from the main loop and switches
 * the tone on and off on time, without blocking.
 */

#include "board.h"
#include "buzzer.h"

#define BUZZER_DDR    DDRD
#define BUZZER_PORT   PORTD
#define BUZZER_PIN    PD3            /* OC2B */

#define BEEP_ON_MS    40             /* length of each beep */

#define OCR_TONE      (F_CPU / (2UL * 32UL * BUZZER_HZ) - 1UL)
#if (OCR_TONE < 1) || (OCR_TONE > 255)
#error "BUZZER_HZ out of range for Timer2 with /32 prescaler"
#endif

static uint16_t period_ms;           /* 0 = silent                    */
static uint32_t beep_start;          /* when the current cycle began  */
static uint8_t  tone_on;

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
    tone_on = 0;
}

/*
 * buzzer_set_period - beep once every `p` ms; 0 stops beeping.
 * A change takes effect at the next cycle, except that going from silent
 * to beeping starts a beep straight away.
 */
void buzzer_set_period(uint16_t p)
{
    if (p != 0 && period_ms == 0) {
        beep_start = 0;                  /* force a new cycle on the next service */
        period_ms = p;
        return;
    }
    period_ms = p;
    if (p == 0 && tone_on) {
        tone_stop();
    }
}

/*
 * buzzer_service - run the beep cadence; call often from the main loop.
 *   start of cycle .. +BEEP_ON_MS : tone on
 *   rest of the cycle             : tone off
 */
void buzzer_service(uint32_t now_ms)
{
    if (period_ms == 0) {
        return;
    }

    uint32_t t = now_ms - beep_start;
    if (beep_start == 0 || t >= period_ms) {
        beep_start = now_ms ? now_ms : 1;    /* 0 is reserved for "restart" */
        tone_start();
    } else if (tone_on && t >= BEEP_ON_MS) {
        tone_stop();
    }
}
