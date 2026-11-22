/*
 * button.h - Push-button on D2 (PD2), to GND, internal pull-up, debounced
 * in the 1 ms tick interrupt.
 */

#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>

void    button_init(void);
void    button_tick(void);          /* from the 1 ms tick ISR only */
uint8_t button_pressed(void);       /* 1 once per debounced press   */

#endif /* BUTTON_H */
