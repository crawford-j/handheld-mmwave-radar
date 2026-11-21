/*
 * font5x7.h - 5x7 pixel font table (printable ASCII), stored in flash.
 */

#ifndef FONT5X7_H
#define FONT5X7_H

#include <stdint.h>
#include <avr/pgmspace.h>

#define FONT_FIRST  0x20    /* first glyph: space */
#define FONT_LAST   0x7E    /* last glyph: '~'    */
#define FONT_COUNT  (FONT_LAST - FONT_FIRST + 1)
#define FONT_W      5       /* glyph width in pixels  */
#define FONT_H      7       /* glyph height in pixels */

extern const uint8_t font5x7[FONT_COUNT][FONT_W] PROGMEM;

#endif /* FONT5X7_H */
