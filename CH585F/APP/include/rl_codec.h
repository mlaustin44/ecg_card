/* Radio link framing (portable, no WCH headers): CRC-16/CCITT-FALSE, COBS
 * and the frame layout of stream_fmt.h / link_fmt.h:
 *
 *   wire  = COBS(frame) + 0x00
 *   frame = u8 type, u8 seq, u16 len (LE), payload[len], u16 crc (LE)
 *   crc   = CRC-16/CCITT-FALSE over type, seq, len and payload
 *
 * Compiled both into the CH585 firmware and into host/radio_tests. Names
 * carry an rl_ prefix so they never collide with the MSPM0's stream.c. */
#ifndef RL_CODEC_H
#define RL_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "stream_fmt.h"

#define RL_HDR_LEN      4u
#define RL_CRC_LEN      2u
#define RL_MAX_PAYLOAD  STREAM_MAX_PAYLOAD                    /* 512 */
#define RL_FRAME_MAX    (RL_HDR_LEN + RL_MAX_PAYLOAD + RL_CRC_LEN) /* 518 */
#define RL_COBS_MAX(n)  ((n) + (n) / 254u + 1u)
/* Largest COBS block of one frame, without the 0x00 delimiter (521). */
#define RL_ENC_MAX      RL_COBS_MAX(RL_FRAME_MAX)
/* Largest frame on the wire, delimiter included (522). */
#define RL_WIRE_MAX     (RL_ENC_MAX + 1u)
/* Wire size of a frame with an n-byte payload, delimiter included. */
#define RL_WIRE_SIZE(n) (RL_COBS_MAX(RL_HDR_LEN + (n) + RL_CRC_LEN) + 1u)

#define RL_CRC_INIT     0xFFFFu
#define RL_COBS_ERROR   ((size_t)-1)

/* CRC-16/CCITT-FALSE (poly 0x1021, no reflection, xorout 0). Start with
 * RL_CRC_INIT; may be chained. */
uint16_t rl_crc16(const uint8_t *p, size_t n, uint16_t crc);

/* COBS-encode n bytes into out (room for RL_COBS_MAX(n)). Returns the
 * encoded length; the output contains no 0x00. */
size_t rl_cobs_encode(const uint8_t *in, size_t n, uint8_t *out);

/* Decode one COBS block (delimiter not included) into out (max bytes).
 * Returns the decoded length, or RL_COBS_ERROR if the block is malformed
 * (zero byte inside, code running past the end) or does not fit. */
size_t rl_cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t max);

/* Build the wire bytes (COBS + 0x00) of one frame into out (out_size bytes).
 * Returns the wire length, or 0 if len > RL_MAX_PAYLOAD or out is too
 * small (RL_WIRE_SIZE(len) bytes are always enough). */
size_t rl_frame_encode(uint8_t type, uint8_t seq, const uint8_t *payload, size_t len,
                       uint8_t *out, size_t out_size);

typedef struct {
    uint8_t type;
    uint8_t seq;
    uint16_t len;
    const uint8_t *payload; /* points into the decoded buffer */
} rl_frame_t;

typedef enum {
    RL_FRAME_OK = 0,
    RL_FRAME_SHORT,    /* fewer than header + CRC bytes */
    RL_FRAME_LEN,      /* len field disagrees with the decoded size, or > 512 */
    RL_FRAME_CRC       /* CRC mismatch */
} rl_frame_status_t;

/* Check a decoded (un-COBSed) frame of n bytes and fill out on success. */
rl_frame_status_t rl_frame_parse(const uint8_t *f, size_t n, rl_frame_t *out);

#endif
