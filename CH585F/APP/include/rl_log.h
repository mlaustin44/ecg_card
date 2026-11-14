/* Device log on the Log characteristic (link_fmt.h, STREAM_LOG): what goes
 * into the notification queue (rl_queue.c) and what leaves it. Portable C,
 * no WCH headers; tested on the host (host/radio_tests/test_log.c).
 *
 * Two writers share Log: the MCU's tee (its RTT text as STREAM_LOG chunks
 * of <= 32 bytes; a line's chunks are consecutive link frames) and the
 * radio's own lines ("radio: " + text + '\n', chunked here). Log text has
 * the lowest priority and is best-effort:
 * - in: a line is started only while the queue holds <= RL_LOG_START_MAX
 *   entries (half of it); its further chunks go in while it holds fewer
 *   than RL_LOG_CONT_MAX, else the rest of that line is dropped. Log text
 *   never makes the queue evict anything. A gap in the link seq inside an
 *   MCU line means its rest was lost: the chunks up to the next line end
 *   are dropped (the MCU starts its next text with '\n' when it cut a line
 *   itself, which is taken as a line start).
 * - out: each queued log entry carries its chunk index within its line in
 *   the upper bits of the entry kind. A chunk whose predecessor was not
 *   sent (evicted by data, or a notification error) is skipped with the
 *   rest of its line, and a head whose tail never went out is ended by a
 *   "\n" notification before the next line. The phone never gets a tail
 *   without its start.
 * - the radio's own line waits (one line, later ones are dropped) while an
 *   MCU line is open and goes in when it ends, or once no MCU chunk came
 *   for RL_LOG_STALE_MS (that MCU line is then given up).
 * Everything dropped is counted, nothing is retried. Task context only. */
#ifndef RL_LOG_H
#define RL_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rl_queue.h"

#define RL_LOG_KIND_MASK 0x0Fu /* queue kind = characteristic | chunk index << 4 */
#define RL_LOG_IDX_SHIFT 4u
#define RL_LOG_START_MAX (RL_Q_SLOTS / 2u)
#define RL_LOG_CONT_MAX  (RL_Q_SLOTS * 3u / 4u)
#define RL_LOG_STALE_MS  50u
#define RL_LOG_OWN_MAX   120u /* own line text, without prefix and '\n' */
#define RL_LOG_PREFIX    "radio: "

typedef struct {
    uint8_t kind;        /* queue kind of the Log characteristic (< 16) */
    /* in: the MCU stream */
    uint8_t in_idx;      /* chunk index of the next MCU chunk; 0 = a line starts */
    bool in_dropping;    /* dropping the rest of the current MCU line */
    bool in_abandoned;   /* the open MCU line was given up (stale) */
    bool in_have_seq;
    uint8_t in_seq;      /* link seq of the last MCU chunk */
    uint32_t in_ms;      /* ... and when it came */
    /* the radio's own line waiting for the MCU line to end */
    uint8_t own_n;
    bool own_waiting;
    char own[RL_LOG_OWN_MAX];
    /* out */
    bool tx_open;        /* the last log text sent did not end with '\n' */
    uint8_t tx_next;     /* chunk index allowed next (0xFF: only a line start) */
    /* counters */
    uint32_t in_chunks, in_drops;   /* MCU chunks queued / dropped */
    uint32_t own_lines, own_drops;  /* own lines queued / dropped */
    uint32_t tx_chunks, tx_skips;   /* log entries notified / skipped on the way out */
} rl_log_t;

void rl_log_init(rl_log_t *l, uint8_t kind);

/* An MCU STREAM_LOG frame (its link seq and payload). on: a phone has Log
 * enabled; when off nothing is queued or counted, but line boundaries are
 * still followed. */
void rl_log_mcu(rl_log_t *l, rl_queue_t *q, bool on, uint8_t seq, const uint8_t *p, size_t n,
                uint32_t now_ms);

/* One of the radio's own log lines (text without '\n', longer than
 * RL_LOG_OWN_MAX is cut). */
void rl_log_own(rl_log_t *l, rl_queue_t *q, bool on, const char *text, size_t n, uint32_t now_ms);

/* Periodic (link tick): lets a waiting own line in after RL_LOG_STALE_MS. */
void rl_log_poll(rl_log_t *l, rl_queue_t *q, bool on, uint32_t now_ms);

/* true while an MCU line is open (own lines wait). */
bool rl_log_mcu_open(const rl_log_t *l);

/* ---- way out: for a queue entry whose kind & RL_LOG_KIND_MASK is l->kind */
typedef enum {
    RL_LOG_TX_SEND,    /* notify the entry, then rl_log_tx_sent(l, e) and pop */
    RL_LOG_TX_NEWLINE, /* notify "\n" first, then rl_log_tx_sent(l, NULL); keep the entry */
    RL_LOG_TX_SKIP     /* rl_log_tx_skip(l) and discard the entry */
} rl_log_tx_t;
rl_log_tx_t rl_log_tx(const rl_log_t *l, const rl_qent_t *e);
void rl_log_tx_sent(rl_log_t *l, const rl_qent_t *e); /* e NULL: the "\n" */
void rl_log_tx_skip(rl_log_t *l);
/* A log entry was dropped on the way out (notification error). */
void rl_log_tx_lost(rl_log_t *l);
/* The queue was flushed (connect, disconnect, shutdown). */
void rl_log_tx_reset(rl_log_t *l);

#endif
