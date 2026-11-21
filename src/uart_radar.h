/*
 * uart_radar.h - HLK-LD2450 link on USART0: RX interrupt, ring buffer and
 * frame parser.
 */

#ifndef UART_RADAR_H
#define UART_RADAR_H

#include <stdint.h>

/* Link speed. The radar runs at its factory 256000 (the only rate where it
 * sends ~11 frames/s); 250000 is the closest the AVR can make (-2.34 %).
 * 38400/57600 work too but the radar only sends ~5 frames/s there. */
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

/* Command words [LD2450 §2.2] */
#define RADAR_CMD_ENABLE_CFG  0x00FF   /* value 0x0001       [§2.2.1] */
#define RADAR_CMD_END_CFG     0x00FE   /* no value           [§2.2.2] */
#define RADAR_CMD_SET_BAUD    0x00A1   /* value: baud index  [§2.2.7] */
#define RADAR_CMD_RESTART     0x00A3   /* no value           [§2.2.9] */
#define RADAR_ACK_BIT         0x0100   /* ACK word = cmd | 0x0100     */
#define RADAR_BAUD_IDX_38400  0x0003   /* [LD2450 Table 6]            */

void    radar_init(void);
void    radar_set_baud(uint32_t baud);
uint8_t radar_poll(radar_frame_t *out);
void    radar_get_stats(radar_stats_t *out);
void    radar_send_cmd(uint16_t cmd, const uint8_t *value, uint8_t vlen);
uint8_t radar_get_ack(uint16_t *cmd, uint16_t *status);

#endif /* UART_RADAR_H */
