/*
 * trig.c - Integer math helpers.
 *
 * Floating point is avoided: avr-libc's float sqrt pulls in about 1 KB of
 * soft-float code and takes hundreds of cycles per call.
 */

#include "trig.h"

/*
 * isqrt32 - floor(sqrt(n)) for a 32-bit unsigned n.
 *
 * Digit-by-digit (binary) method: `bit` walks down the even powers of 4.
 * At each step, if the remainder can absorb (res + bit), that bit of the
 * root is 1. Exactly 16 iterations; shifts and adds only.
 *
 * Used for target distance: x, y are within +/-32767 mm, so x*x + y*y is
 * below 2^31 and fits in uint32_t.
 */
uint16_t isqrt32(uint32_t n)
{
    uint32_t res = 0;
    uint32_t bit = 1UL << 30;           /* highest power of 4 in 32 bits */

    while (bit > n) {
        bit >>= 2;
    }
    while (bit) {
        if (n >= res + bit) {
            n  -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (uint16_t)res;
}

/*
 * sin_q15 - sin(deg) x 32767 for any whole-degree angle.
 *
 * Only 0..90 degrees are stored (91 x 2 B in flash). The rest follows from:
 *   sin(-a)      = -sin(a)
 *   sin(180 - a) =  sin(a)
 */
int16_t sin_q15(int16_t deg)
{
    deg %= 360;                         /* -359..359 */
    if (deg > 180) {
        deg -= 360;
    } else if (deg < -180) {
        deg += 360;
    }                                   /* now -180..180 */

    int8_t sign = 1;
    if (deg < 0) {
        deg = -deg;
        sign = -1;
    }                                   /* now 0..180 */
    if (deg > 90) {
        deg = 180 - deg;                /* 0..90 */
    }

    int16_t v = (int16_t)pgm_read_word(&sine_q15_table[deg]);
    return sign > 0 ? v : (int16_t)-v;
}

/*
 * cos_q15 - cos(deg) x 32767, using cos(a) = sin(90 - a).
 */
int16_t cos_q15(int16_t deg)
{
    return sin_q15(90 - deg);
}
