/*
 * uart_radar.h - HLK-LD2450 link on USART0: RX interrupt, ring buffer and
 * frame parser.
 */

#ifndef UART_RADAR_H
#define UART_RADAR_H

#include <stdint.h>

/* Link speed. 250000 talks to a factory-default radar (256000, -2.34 %);
 * 38400 is the target rate after the one-time baud switch. */
#ifndef RADAR_BAUD
#define RADAR_BAUD 250000UL
#endif

#define RADAR_TARGETS       3
#define RADAR_TARGET_BYTES  8
#define RADAR_PAYLOAD       (RADAR_TARGETS * RADAR_TARGET_BYTES)   /* 24 */

typedef struct {
    uint8_t  valid;     /* 0 = slot empty (all 8 bytes zero)   */
    int16_t  x;         /* mm, lateral                          */
    int16_t  y;         /* mm, away from the antenna            */
    int16_t  speed;     /* cm/s, radial                         */
    uint16_t res;       /* mm, distance-gate size (not distance) */
} radar_target_t;

typedef struct {
    uint8_t        raw[RADAR_PAYLOAD];   /* payload exactly as received */
    radar_target_t t[RADAR_TARGETS];
} radar_frame_t;

typedef struct {
    uint16_t frames_ok;     /* frames with a correct tail            */
    uint16_t frames_bad;    /* header found but tail did not match   */
    uint16_t fe;            /* USART framing errors (bad stop bit)   */
    uint16_t dor;           /* USART data overruns (byte lost in HW) */
    uint16_t ovf;           /* ring buffer full, byte dropped        */
} radar_stats_t;

void    radar_init(void);
uint8_t radar_poll(radar_frame_t *out);
void    radar_get_stats(radar_stats_t *out);

#endif /* UART_RADAR_H */
