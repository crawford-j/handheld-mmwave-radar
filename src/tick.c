/*
 * tick.c - 1 ms system tick using Timer0 in CTC mode.
 *
 * Tick rate:
 *   f = F_CPU / (prescaler * (1 + OCR0A)) = 16 MHz / (64 * 250) = 1000 Hz
 * so OCR0A = 249 with clk/64 [M328P §15.7.2].
 * Counter wraps after 2^32 ms = 49.7 days; elapsed-time checks use
 * unsigned subtraction (now - then), which stays correct across the wrap.
 */

#include "board.h"
#include <avr/interrupt.h>
#include <util/atomic.h>
#include "tick.h"
#include "buzzer.h"

static volatile uint32_t ms_count;

/*
 * tick_init - start Timer0 as a 1 kHz interrupt source.
 * Interrupts must be enabled globally (sei) by the caller.
 */
void tick_init(void)
{
    /* WGM01:0 = 10 -> CTC mode, TOP = OCR0A; COM0A/B = 00, pins unused
     * [M328P §15.9.1, Table 15-8]. */
    TCCR0A = (1 << WGM01);
    /* CS02:0 = 011 -> clk/64; WGM02 = 0 [M328P §15.9.2, Table 15-9]. */
    TCCR0B = (1 << CS01) | (1 << CS00);
    OCR0A  = 249;                       /* 250 counts -> 1 ms */
    TCNT0  = 0;
    /* OCIE0A = 1: interrupt on compare match A [M328P §15.9.6]. */
    TIMSK0 = (1 << OCIE0A);
}

/*
 * Timer0 compare match A: fires every 1 ms. Also drives the buzzer's beep
 * timing, so beeps stay regular even while the main loop is drawing.
 */
ISR(TIMER0_COMPA_vect)
{
    ms_count++;
    buzzer_tick();
}

/*
 * tick_ms - milliseconds since tick_init().
 * The 32-bit counter is copied with interrupts off, because the AVR reads it
 * one byte at a time and the ISR could change it halfway through.
 */
uint32_t tick_ms(void)
{
    uint32_t now;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        now = ms_count;
    }
    return now;
}
