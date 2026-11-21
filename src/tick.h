/*
 * tick.h - 1 ms system tick on Timer0.
 */

#ifndef TICK_H
#define TICK_H

#include <stdint.h>

void     tick_init(void);
uint32_t tick_ms(void);

#endif /* TICK_H */
