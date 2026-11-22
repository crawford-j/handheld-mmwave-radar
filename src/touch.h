/*
 * touch.h - XPT2046 resistive touch controller on the shared SPI bus.
 */

#ifndef TOUCH_H
#define TOUCH_H

#include <stdint.h>

/* ---- Calibration (from the TOUCH_CAL=1 build, see touch_cal.c) ----
 * TOUCH_SWAP_XY : 1 if the controller's X channel runs along the screen's
 *                 vertical axis (landscape rotation of a portrait panel).
 * TOUCH_X0/X1   : raw reading at screen x = 0 and x = 319.
 * TOUCH_Y0/Y1   : raw reading at screen y = 0 and y = 239.
 * A reversed axis just has X0 > X1 (or Y0 > Y1); the mapping handles it.
 * Measured on this panel with TOUCH_CAL=1 on 2026-11-22: the controller's
 * X channel runs along the screen's vertical axis (portrait panel used in
 * landscape), and neither axis is reversed. */
#ifndef TOUCH_SWAP_XY
#define TOUCH_SWAP_XY  1
#define TOUCH_X0       320
#define TOUCH_X1       3906
#define TOUCH_Y0       234
#define TOUCH_Y1       3825
#endif

/* Pressure threshold on Z1: higher Z1 means a firmer touch [XPT2046
 * "Pressure Measurement"]. Below this the panel counts as not touched. */
#define TOUCH_Z_MIN    150

typedef struct {
    uint16_t x, y;          /* raw 12-bit position channels   */
    uint16_t z1, z2;        /* raw 12-bit pressure channels   */
} touch_raw_t;

void    touch_init(void);
uint8_t touch_read_raw(touch_raw_t *r);
uint8_t touch_read(int16_t *sx, int16_t *sy);

#endif /* TOUCH_H */
