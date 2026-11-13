/* Byte-at-a-time receiver for the radio link (portable, no WCH headers).
 *
 * Feed every received byte; a 0x00 ends a COBS block, which is decoded and
 * checked (length field, CRC). Garbage, truncated frames (a delimiter in
 * the middle of a frame) and oversize blocks are counted and dropped; the
 * parser always resynchronises on the next 0x00. Empty blocks (0x00 0x00,
 * e.g. a wake byte) are ignored. Sequence gaps are counted over valid
 * frames only. */
#ifndef RL_PARSER_H
#define RL_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rl_codec.h"

typedef struct {
    uint32_t frames;   /* valid frames delivered */
    uint32_t crc_err;  /* CRC mismatch */
    uint32_t cobs_err; /* malformed COBS block */
    uint32_t len_err;  /* too short, or len field disagrees with size */
    uint32_t oversize; /* block longer than the largest legal frame */
    uint32_t seq_gaps; /* frames missing according to seq */
    uint32_t empty;    /* empty blocks (consecutive delimiters) */
} rl_parser_stats_t;

typedef struct {
    uint8_t enc[RL_ENC_MAX];   /* COBS bytes of the frame being received */
    uint8_t dec[RL_FRAME_MAX]; /* decoded frame; rl_frame_t.payload points here */
    size_t n;
    bool overflow;             /* discarding until the next delimiter */
    bool have_seq;
    uint8_t next_seq;
    rl_parser_stats_t st;
} rl_parser_t;

void rl_parser_init(rl_parser_t *p);

/* Returns true when byte completes a valid frame; *out is then valid until
 * the next call. */
bool rl_parser_feed(rl_parser_t *p, uint8_t byte, rl_frame_t *out);

#endif
