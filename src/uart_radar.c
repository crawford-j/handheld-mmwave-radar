/*
 * uart_radar.c - HLK-LD2450 radar link on USART0.
 *
 * Data path:
 *   USART RX interrupt -> 128-byte ring buffer -> radar_poll() state machine
 *   in the main loop -> decoded frame.
 *
 * Frame format [LD2450 §2.3 Table 9]:
 *   AA FF 03 00 | target1 (8 B) | target2 (8 B) | target3 (8 B) | 55 CC
 * Each target [LD2450 Table 10], little-endian [LD2450 §2.1.1]:
 *   X int16 mm | Y int16 mm | speed int16 cm/s | gate size uint16 mm
 * X, Y and speed are sign-magnitude with an inverted sign bit: bit 15 = 1
 * means positive, bit 15 = 0 means negative; bits 14..0 are the magnitude.
 *
 * Buffer sizing: the radar sends 30 B x 10 frames/s = 300 B/s whatever the
 * baud rate, so 128 B holds about 0.4 s of data. That covers the longest
 * blocking screen update in the main loop.
 */

#include "board.h"
#include <avr/interrupt.h>
#include <util/atomic.h>
#include "uart_radar.h"

/* ---- Baud rate [M328P §20.3 Table 20-1], normal speed (U2X0 = 0) ----
 *   UBRR0 = F_CPU / (16 * baud) - 1, rounded to nearest
 *   250000: 16e6 / (16 * 250000) - 1 = 3     -> exactly 250000 (-2.34 % vs 256000)
 *   38400:  16e6 / (16 * 38400)  - 1 = 25.04 -> 25 -> 38461.5 (+0.16 %)
 * U2X0 = 0 is used because 16x oversampling tolerates more baud error than
 * 8x: +/-2.0 % recommended vs +/-1.5 % [M328P Tables 20-2, 20-3]. */
#define UBRR_VALUE  ((F_CPU + 8UL * RADAR_BAUD) / (16UL * RADAR_BAUD) - 1UL)

#if (RADAR_BAUD != 250000UL) && (RADAR_BAUD != 38400UL)
#error "RADAR_BAUD must be 250000 or 38400"
#endif

/* ---- Ring buffer (single producer = ISR, single consumer = main) ----
 * Indices are 8-bit so reads and writes are atomic on the AVR; the ISR only
 * writes head, the main loop only writes tail. Size must be a power of 2. */
#define RB_SIZE  128
#define RB_MASK  (RB_SIZE - 1)

static volatile uint8_t rb_buf[RB_SIZE];
static volatile uint8_t rb_head;        /* next slot the ISR writes */
static volatile uint8_t rb_tail;        /* next slot main reads     */

/* Error counters written by the ISR. 16-bit, so read them atomically. */
static volatile uint16_t cnt_fe, cnt_dor, cnt_ovf;

/* Counters written only by the main loop. */
static uint16_t cnt_ok, cnt_bad;

/* ---- Parser state ---- */
static const uint8_t HDR[4]  = { 0xAA, 0xFF, 0x03, 0x00 };
static const uint8_t TAIL[2] = { 0x55, 0xCC };

typedef enum { ST_HEADER, ST_PAYLOAD, ST_TAIL } parse_state_t;

static parse_state_t state = ST_HEADER;
static uint8_t       idx;                       /* position within the state */
static uint8_t       scratch[RADAR_PAYLOAD];    /* frame being received      */

/*
 * radar_init - USART0 at RADAR_BAUD, 8N1, RX interrupt on.
 */
void radar_init(void)
{
    UBRR0H = (uint8_t)(UBRR_VALUE >> 8);
    UBRR0L = (uint8_t)(UBRR_VALUE & 0xFF);

    /* U2X0 = 0 (normal speed), MPCM0 = 0 [M328P §20.11.2]. */
    UCSR0A = 0;

    /* RXEN0  = 1: receiver on (PD0 becomes RXD)
     * TXEN0  = 1: transmitter on (PD1 becomes TXD and idles high, so the
     *             radar's RX line is never left floating)
     * RXCIE0 = 1: interrupt on each received byte [M328P §20.11.3] */
    UCSR0B = (1 << RXEN0) | (1 << TXEN0) | (1 << RXCIE0);

    /* UMSEL01:0 = 00 async, UPM01:0 = 00 no parity, USBS0 = 0 one stop bit,
     * UCSZ01:0 = 11 (UCSZ02 = 0 in UCSR0B) -> 8 data bits [M328P §20.11.4]. */
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

/*
 * USART0 receive complete: move one byte into the ring buffer.
 * The error flags in UCSR0A belong to the byte in UDR0, so they must be read
 * before UDR0; reading UDR0 moves on to the next byte [M328P §20.7.4].
 */
ISR(USART_RX_vect)
{
    uint8_t status = UCSR0A;
    uint8_t data   = UDR0;

    if (status & (1 << FE0)) {          /* stop bit was 0: baud mismatch or noise */
        cnt_fe++;
    }
    if (status & (1 << DOR0)) {         /* a byte arrived before the last was read */
        cnt_dor++;
    }

    uint8_t next = (rb_head + 1) & RB_MASK;
    if (next == rb_tail) {              /* full: drop the byte, parser resyncs */
        cnt_ovf++;
        return;
    }
    rb_buf[rb_head] = data;
    rb_head = next;
}

/*
 * decode_axis - convert an LD2450 sign-magnitude word to a signed integer.
 * Bit 15 set = positive, clear = negative [LD2450 §2.3 Table 10].
 * Datasheet example: 0x86B1 -> +1713, 0x030E -> -782.
 */
static int16_t decode_axis(uint8_t lo, uint8_t hi)
{
    uint16_t raw = (uint16_t)lo | ((uint16_t)hi << 8);
    int16_t  mag = (int16_t)(raw & 0x7FFF);
    return (raw & 0x8000) ? mag : (int16_t)-mag;
}

/*
 * decode_frame - fill `out` from the 24 payload bytes in scratch[].
 */
static void decode_frame(radar_frame_t *out)
{
    for (uint8_t i = 0; i < RADAR_PAYLOAD; i++) {
        out->raw[i] = scratch[i];
    }

    for (uint8_t n = 0; n < RADAR_TARGETS; n++) {
        const uint8_t *p = &scratch[n * RADAR_TARGET_BYTES];
        radar_target_t *t = &out->t[n];

        /* An absent target is sent as 8 zero bytes [LD2450 §2.3 example]. */
        uint8_t any = 0;
        for (uint8_t k = 0; k < RADAR_TARGET_BYTES; k++) {
            any |= p[k];
        }
        t->valid = (any != 0);

        t->x     = decode_axis(p[0], p[1]);
        t->y     = decode_axis(p[2], p[3]);
        t->speed = decode_axis(p[4], p[5]);
        t->res   = (uint16_t)p[6] | ((uint16_t)p[7] << 8);
    }
}

/*
 * parse_byte - advance the frame state machine by one byte.
 * Returns 1 when a complete frame with a valid tail is in scratch[].
 *
 *   ST_HEADER : match AA FF 03 00 one byte at a time. On a mismatch, restart,
 *               but if the bad byte is itself 0xAA, count it as header byte 1
 *               so "AA AA FF 03 00" still syncs.
 *   ST_PAYLOAD: copy 24 bytes into scratch[].
 *   ST_TAIL   : expect 55 CC. Anything else drops the frame (frames_bad++).
 * A frame is only handed out after its tail checks, so a half-received or
 * misaligned frame is never used.
 */
static uint8_t parse_byte(uint8_t b)
{
    switch (state) {
    case ST_HEADER:
        if (b == HDR[idx]) {
            if (++idx == sizeof(HDR)) {
                state = ST_PAYLOAD;
                idx = 0;
            }
        } else {
            idx = (b == HDR[0]) ? 1 : 0;
        }
        break;

    case ST_PAYLOAD:
        scratch[idx++] = b;
        if (idx == RADAR_PAYLOAD) {
            state = ST_TAIL;
            idx = 0;
        }
        break;

    case ST_TAIL:
        if (b != TAIL[idx]) {
            cnt_bad++;
            state = ST_HEADER;
            idx = (b == HDR[0]) ? 1 : 0;
            break;
        }
        if (++idx == sizeof(TAIL)) {
            cnt_ok++;
            state = ST_HEADER;
            idx = 0;
            return 1;
        }
        break;
    }
    return 0;
}

/*
 * radar_poll - drain the ring buffer through the parser.
 * Returns 1 and fills `out` as soon as a complete frame is parsed; any bytes
 * after it stay in the buffer for the next call. Returns 0 if the buffer
 * empties first.
 */
uint8_t radar_poll(radar_frame_t *out)
{
    while (rb_tail != rb_head) {
        uint8_t b = rb_buf[rb_tail];
        rb_tail = (rb_tail + 1) & RB_MASK;

        if (parse_byte(b)) {
            decode_frame(out);
            return 1;
        }
    }
    return 0;
}

/*
 * radar_get_stats - copy all link counters. The ISR-owned counters are
 * 16-bit, so they are copied with interrupts off.
 */
void radar_get_stats(radar_stats_t *out)
{
    out->frames_ok  = cnt_ok;
    out->frames_bad = cnt_bad;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        out->fe  = cnt_fe;
        out->dor = cnt_dor;
        out->ovf = cnt_ovf;
    }
}
