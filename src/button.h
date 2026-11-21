/*
 * button.h - Push-button on D2 (PD2), to GND, internal pull-up, debounced.
 */

#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>

void    button_init(void);
uint8_t button_pressed(uint32_t now_ms);

#endif /* BUTTON_H */
