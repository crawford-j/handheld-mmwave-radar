/*
 * buzzer.h - Passive buzzer on OC2B (PD3 / D3): Timer2 tone plus a
 * non-blocking beep cadence.
 */

#ifndef BUZZER_H
#define BUZZER_H

#include <stdint.h>

/* Tone frequency. Passive buzzers are loudest near their resonance,
 * usually 2-4 kHz. With the /32 prescaler OCR2A must fit in 8 bits, so the
 * lowest possible tone is 16 MHz / (2 * 32 * 256) = 977 Hz. */
#ifndef BUZZER_HZ
#define BUZZER_HZ 2000UL
#endif

void buzzer_init(void);
void buzzer_set_period(uint16_t period_ms);
void buzzer_service(uint32_t now_ms);

#endif /* BUZZER_H */
