/*
 * uart_radar.c - HLK-LD2450 radar link on USART0.
 *
 * Data path:
 *   USART RX interrupt -> 128-byte ring buffer -> radar_poll() state machine
 *   in the main loop -> decoded frame.
 *
 * Data frame format [LD2450 §2.3 Table 9]:
 *   AA FF 03 00 | target1 (8 B) | target2 (8 B) | target3 (8 B) | 55 CC
 * Each target [LD2450 Table 10], little-endian [LD2450 §2.1.1]:
 *   X int16 mm | Y int16 mm | speed int16 cm/s | gate size uint16 mm
 * X, Y and speed are sign-magnitude with an inverted sign bit: bit 15 = 1
 * means positive, bit 15 = 0 means negative; bits 14..0 are the magnitude.
 *
 * Command / ACK frame format [LD2450 §2.1.2 Tables 2-5]:
 *   FD FC FB FA | length (2 B LE) | command word (2 B LE) + value | 04 03 02 01
 * An ACK echoes the command word with bit 8 set (cmd | 0x0100), followed by
 * a 2-byte status: 0 = success, 1 = failure.
 *
 * Buffer sizing: the radar sends 30 B x 10 frames/s = 300 B/s whatever the
 * baud rate, so 128 B holds about 0.4 s of data. That covers the longest
 * blocking screen update in the main loop.
 */

#include "board.h"
#include <avr/interrupt.h>
#include <util/atomic.h>
#include "uart_radar.h"

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
static const uint8_t HDR[4]      = { 0xAA, 0xFF, 0x03, 0x00 };
static const uint8_t TAIL[2]     = { 0x55, 0xCC };
static const uint8_t ACK_HDR[4]  = { 0xFD, 0xFC, 0xFB, 0xFA };
static const uint8_t ACK_TAIL[4] = { 0x04, 0x03, 0x02, 0x01 };

#define ACK_KEEP  4     /* bytes of ACK data kept: command word + status */
#define ACK_MAX   32    /* longest ACK body accepted (zone query = 30 B) */

typedef enum {
    ST_HEADER,          /* hunting for a data or ACK header */
    ST_PAYLOAD,         /* data frame: 24 target bytes      */
    ST_TAIL,            /* data frame: 55 CC                */
    ST_ACK_LEN,         /* ACK: 2-byte length               */
    ST_ACK_BODY,        /* ACK: `ack_len` bytes             */
    ST_ACK_TAIL         /* ACK: 04 03 02 01                 */
} parse_state_t;

static parse_state_t state = ST_HEADER;
static uint8_t       idx;                       /* position within the state  */
static uint8_t       ack_idx;                   /* ACK header match position  */
static uint8_t       scratch[RADAR_PAYLOAD];    /* data frame being received  */
static uint16_t      ack_len;
static uint8_t       ack_buf[ACK_KEEP];

static uint8_t  ack_ready;                      /* a complete ACK is waiting */
static uint16_t ack_cmd, ack_status;

/*
 * radar_set_baud - (re)program USART0 for `baud`, 8N1, RX interrupt on, and
 * discard anything already received.
 *
 * Normal speed mode (U2X0 = 0) [M328P §20.3 Table 20-1]:
 *   UBRR0 = F_CPU / (16 * baud) - 1, rounded to nearest
 *   250000: 16e6 / (16 * 250000) - 1 = 3     -> exactly 250000 (-2.34 % vs 256000)
 *   38400:  16e6 / (16 * 38400)  - 1 = 25.04 -> 25 -> 38461.5 (+0.16 %)
 * U2X0 = 0 because 16x oversampling tolerates more baud error than 8x:
 * +/-2.0 % recommended vs +/-1.5 % [M328P Tables 20-2, 20-3].
 */
void radar_set_baud(uint32_t baud)
{
    uint16_t ubrr = (uint16_t)((F_CPU + 8UL * baud) / (16UL * baud) - 1UL);

    /* Turn the USART off while the rate changes. */
    UCSR0B = 0;

    UBRR0H = (uint8_t)(ubrr >> 8);
    UBRR0L = (uint8_t)(ubrr & 0xFF);

    /* U2X0 = 0 (normal speed), MPCM0 = 0. Writing TXC0 = 1 clears the
     * transmit-complete flag [M328P §20.11.2]. */
    UCSR0A = (1 << TXC0);

    /* UMSEL01:0 = 00 async, UPM01:0 = 00 no parity, USBS0 = 0 one stop bit,
     * UCSZ01:0 = 11 (UCSZ02 = 0 in UCSR0B) -> 8 data bits [M328P §20.11.4]. */
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);

    /* Empty the ring buffer and restart the parser, so no bytes received at
     * the old rate get mixed into new frames. */
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        rb_head = 0;
        rb_tail = 0;
    }
    state = ST_HEADER;
    idx = 0;
    ack_idx = 0;
    ack_ready = 0;

    /* RXEN0  = 1: receiver on (PD0 becomes RXD)
     * TXEN0  = 1: transmitter on (PD1 becomes TXD and idles high, so the
     *             radar's RX line is never left floating)
     * RXCIE0 = 1: interrupt on each received byte [M328P §20.11.3] */
    UCSR0B = (1 << RXEN0) | (1 << TXEN0) | (1 << RXCIE0);
}

/*
 * radar_init - start the link at the build-time rate RADAR_BAUD.
 */
void radar_init(void)
{
    radar_set_baud(RADAR_BAUD);
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
 * tx_byte - send one byte, waiting until the transmit buffer is free
 * (UDRE0 = 1) [M328P §20.6.1].
 */
static void tx_byte(uint8_t b)
{
    while (!(UCSR0A & (1 << UDRE0))) {
    }
    UDR0 = b;
}

/*
 * radar_send_cmd - send one command frame [LD2450 §2.1.2 Tables 2, 3]:
 *   FD FC FB FA | len = 2 + vlen (LE) | cmd (LE) | value | 04 03 02 01
 * Blocks until the last stop bit has left (TXC0), so the caller can change
 * the baud rate right afterwards without cutting the frame short.
 */
void radar_send_cmd(uint16_t cmd, const uint8_t *value, uint8_t vlen)
{
    uint16_t len = 2 + vlen;

    UCSR0A = (1 << TXC0);               /* clear transmit-complete flag */

    for (uint8_t i = 0; i < sizeof(ACK_HDR); i++) {
        tx_byte(ACK_HDR[i]);            /* command header = same 4 bytes */
    }
    tx_byte(len & 0xFF);
    tx_byte(len >> 8);
    tx_byte(cmd & 0xFF);
    tx_byte(cmd >> 8);
    for (uint8_t i = 0; i < vlen; i++) {
        tx_byte(value[i]);
    }
    for (uint8_t i = 0; i < sizeof(ACK_TAIL); i++) {
        tx_byte(ACK_TAIL[i]);
    }

    while (!(UCSR0A & (1 << TXC0))) {   /* wait for the shift register to empty */
    }
}

/*
 * radar_get_ack - return 1 once for each complete ACK frame received, with
 * its command word (cmd | 0x0100) and status (0 = success).
 */
uint8_t radar_get_ack(uint16_t *cmd, uint16_t *status)
{
    if (!ack_ready) {
        return 0;
    }
    ack_ready = 0;
    *cmd = ack_cmd;
    *status = ack_status;
    return 1;
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
 * go_header - return to header hunting, treating byte `b` as a possible
 * first header byte so a new frame starting right here is not missed.
 */
static void go_header(uint8_t b)
{
    state   = ST_HEADER;
    idx     = (b == HDR[0]) ? 1 : 0;
    ack_idx = (b == ACK_HDR[0]) ? 1 : 0;
}

/*
 * parse_byte - advance the frame state machine by one byte.
 * Returns 1 when a complete data frame with a valid tail is in scratch[].
 * ACK frames are captured into ack_cmd / ack_status instead.
 *
 *   ST_HEADER : match AA FF 03 00 (data) and FD FC FB FA (ACK) in parallel,
 *               one byte at a time. A mismatching byte that equals a first
 *               header byte restarts that match at position 1.
 *   ST_PAYLOAD: copy 24 bytes into scratch[].
 *   ST_TAIL   : expect 55 CC. Anything else drops the frame (frames_bad++).
 *   ST_ACK_*  : length, body (first 4 bytes kept), then 04 03 02 01.
 * A frame is only used after its tail checks, so a half-received or
 * misaligned frame never reaches the caller.
 */
static uint8_t parse_byte(uint8_t b)
{
    switch (state) {
    case ST_HEADER:
        idx     = (b == HDR[idx])         ? idx + 1     : (b == HDR[0]);
        ack_idx = (b == ACK_HDR[ack_idx]) ? ack_idx + 1 : (b == ACK_HDR[0]);
        if (idx == sizeof(HDR)) {
            state = ST_PAYLOAD;
            idx = 0;
        } else if (ack_idx == sizeof(ACK_HDR)) {
            state = ST_ACK_LEN;
            idx = 0;
            ack_len = 0;
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
            go_header(b);
            break;
        }
        if (++idx == sizeof(TAIL)) {
            cnt_ok++;
            go_header(0);
            return 1;
        }
        break;

    case ST_ACK_LEN:
        ack_len |= (uint16_t)b << (8 * idx);    /* little-endian */
        if (++idx == 2) {
            idx = 0;
            state = (ack_len >= ACK_KEEP && ack_len <= ACK_MAX) ? ST_ACK_BODY : ST_HEADER;
            if (state == ST_HEADER) {
                go_header(b);
            }
        }
        break;

    case ST_ACK_BODY:
        if (idx < ACK_KEEP) {
            ack_buf[idx] = b;
        }
        if (++idx == ack_len) {
            state = ST_ACK_TAIL;
            idx = 0;
        }
        break;

    case ST_ACK_TAIL:
        if (b != ACK_TAIL[idx]) {
            go_header(b);
            break;
        }
        if (++idx == sizeof(ACK_TAIL)) {
            ack_cmd    = (uint16_t)ack_buf[0] | ((uint16_t)ack_buf[1] << 8);
            ack_status = (uint16_t)ack_buf[2] | ((uint16_t)ack_buf[3] << 8);
            ack_ready  = 1;
            go_header(0);
        }
        break;
    }
    return 0;
}

/*
 * radar_poll - drain the ring buffer through the parser.
 * Returns 1 and fills `out` as soon as a complete data frame is parsed; any
 * bytes after it stay in the buffer for the next call. Returns 0 if the
 * buffer empties first. ACKs are picked up with radar_get_ack().
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
