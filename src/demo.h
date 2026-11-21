/*
 * demo.h - Fake radar frames for testing the display without the radar.
 * Built only with `make DEMO=1`.
 */

#ifndef DEMO_H
#define DEMO_H

#include <stdint.h>
#include "uart_radar.h"

uint8_t demo_frame(uint32_t now_ms, radar_frame_t *fr);

#endif /* DEMO_H */
