/* Stream output (RTT channel 1; a tap forwards to the radio link): COBS + CRC-16/
 * CCITT-FALSE framing per fw/include/stream_fmt.h, one global sequence
 * counter, typed emitters. Frames that hal_stream_write() does not accept
 * are counted as dropped, never retried and never waited for. */
#ifndef APP_STREAM_H
#define APP_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Largest payload this build sends (STREAM_MAX_PAYLOAD allows 512; the
 * biggest frame here is ECG_BLOCK, 25 bytes). */
#define STREAM_APP_MAX_PAYLOAD 64u
#define STREAM_FRAME_MAX (4u + STREAM_APP_MAX_PAYLOAD + 2u)
#define COBS_MAX(n) ((n) + (n) / 254u + 1u)

/* ---- pure helpers (unit-tested) ---- */
uint16_t crc16_ccitt_false(const uint8_t *p, size_t n, uint16_t crc); /* init 0xFFFF */
/* Encode n bytes into out (room for COBS_MAX(n)); returns the length. The
 * result contains no 0x00. */
size_t cobs_encode(const uint8_t *in, size_t n, uint8_t *out);
/* Decode one COBS block (without the delimiter); returns the length or
 * (size_t)-1 if malformed / does not fit in max. */
size_t cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t max);
/* Build the wire bytes of one frame (COBS + 0x00) into out; returns the
 * length (0 if payload too long). */
size_t stream_build(uint8_t type, uint8_t seq, const uint8_t *payload, size_t len,
                    uint8_t out[COBS_MAX(STREAM_FRAME_MAX) + 1u]);

/* ---- output ---- */
void stream_init(void);
/* Tap: called with every frame stream_send() produces (whether or not RTT
 * took it), before it is written. The radio link uses it to forward data to
 * a connected phone. stream_init() keeps the tap; NULL removes it. */
typedef void (*stream_tap_t)(uint8_t type, const uint8_t *payload, size_t len);
void stream_set_tap(stream_tap_t tap);
bool stream_send(uint8_t type, const uint8_t *payload, size_t len);
uint32_t stream_frames(void);  /* accepted */
uint32_t stream_drops(void);   /* rejected by hal_stream_write() */

void stream_ecg_block(uint32_t start, uint8_t flags, const int16_t *s);
void stream_ppg_block(uint32_t start, uint16_t led_code, uint8_t flags, const int16_t *cf);
void stream_ev_r_peak(uint32_t index, uint16_t hr_bpm);
void stream_ev_pulse(uint32_t foot_index, uint16_t pulse_bpm, uint16_t pat_ms, uint16_t pi);
void stream_ev_lead(bool contact);
void stream_ev_state(uint8_t mode, uint8_t notch_hz, uint16_t mid_code, uint8_t batt_pct,
                     uint8_t flags);
void stream_ev_button(uint8_t button, uint8_t kind);
/* STREAM_EV_CMD_RESULT (link_fmt.h): outcome of a phone command. */
void stream_ev_cmd_result(uint8_t cmd, uint8_t result);

#endif
