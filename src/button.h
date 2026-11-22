/*
 * button.h - Push-button on D2 (PD2), to GND, internal pull-up, debounced
 * in the 1 ms tick interrupt. Distinguishes short and long (1.5 s) presses.
 */

#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>

#define BTN_NONE    0
#define BTN_SHORT   1           /* released within 1.5 s */
#define BTN_LONG    2           /* held for 1.5 s        */

void    button_init(void);
void    button_tick(void);          /* from the 1 ms tick ISR only */
uint8_t button_event(void);         /* BTN_NONE / BTN_SHORT / BTN_LONG */

#endif /* BUTTON_H */
