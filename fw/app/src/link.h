/* Radio link layer (fw/include/link_fmt.h) on UART0: frame output with the
 * MCU -> radio sequence counter, a byte-wise RX parser and LINK_CMD
 * validation. The RX parser and link_cmd_parse() are pure (unit-tested);
 * link_send() writes to hal_uart_write() and never waits: a frame that does
 * not fit in the TX ring is dropped and counted (its seq is used up, so the
 * radio sees the gap). */
#ifndef APP_LINK_H
#define APP_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link_fmt.h"
#include "stream.h"

/* Longest radio -> MCU payload accepted (STREAM_LOG text is the long one;
 * longer frames are dropped and counted as bad). */
#define LINK_RX_MAX_PAYLOAD 120u
#define LINK_RX_MAX_WIRE    COBS_MAX(LINK_RX_MAX_PAYLOAD + 6u)

/* ---- output ---- */
void link_init(void);      /* seq and counters to 0 (boot) */
void link_tx_reset(void);  /* seq back to 0 (each radio power-up) */
bool link_send(uint8_t type, const uint8_t *payload, size_t len);
uint8_t link_tx_last_seq(void);
uint32_t link_tx_frames(void); /* accepted by hal_uart_write() */
uint32_t link_tx_drops(void);  /* did not fit */

/* ---- input ---- */
typedef struct {
    uint32_t frames;       /* good frames delivered */
    uint32_t crc_errors;   /* well-formed COBS, CRC mismatch */
    uint32_t bad_frames;   /* COBS error, length mismatch, too long, too short */
    uint32_t seq_gaps;     /* radio seq not previous + 1 (frames lost on the way) */
    uint32_t unknown_types;/* counted by the caller */
} link_rx_stats_t;

typedef void (*link_frame_fn)(void *ctx, uint8_t type, uint8_t seq, const uint8_t *payload,
                              size_t len);

typedef struct {
    uint8_t buf[LINK_RX_MAX_WIRE];
    size_t n;
    bool overflow;
    bool have_seq;
    uint8_t seq;
    link_rx_stats_t st;
} link_rx_t;

void link_rx_init(link_rx_t *r);  /* also clears the counters */
void link_rx_restart(link_rx_t *r); /* new radio boot: drop a partial frame, forget seq */
/* Feed received bytes; fn is called for every good frame, in order. */
void link_rx_feed(link_rx_t *r, const uint8_t *b, size_t n, link_frame_fn fn, void *ctx);

/* ---- commands ---- */
typedef struct {
    uint8_t cmd;
    uint8_t mode;           /* START: 1 ECG, 2 PPG, 3 both */
    uint8_t item, value;    /* SET */
    uint32_t unix_s;        /* SET_TIME */
    int16_t utc_offset_min; /* SET_TIME */
} link_cmd_t;
#define LINK_TIME_MIN      1704067200u /* 2024-01-01: anything earlier is a client bug */
#define LINK_UTC_OFF_MIN   (-720)
#define LINK_UTC_OFF_MAX   840
/* Syntax and range check of a LINK_CMD payload: LINK_RES_OK (out filled),
 * LINK_RES_UNKNOWN (unknown cmd) or LINK_RES_BAD_ARG (empty, wrong length,
 * value out of range). out->cmd is set whenever n > 0. */
uint8_t link_cmd_parse(const uint8_t *p, size_t n, link_cmd_t *out);

#endif
