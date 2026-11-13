/* Radio link framing: CRC-16/CCITT-FALSE, COBS, frame encode/check.
 * Portable C (no WCH headers); tested on the host by host/radio_tests. */
#include "rl_codec.h"

uint16_t rl_crc16(const uint8_t *p, size_t n, uint16_t crc)
{
    while (n--) {
        unsigned b;
        crc ^= (uint16_t)((uint16_t)*p++ << 8);
        for (b = 0; b < 8u; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

size_t rl_cobs_encode(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t i, o = 1, code_at = 0;
    uint8_t code = 1;

    for (i = 0; i < n; i++) {
        if (in[i] == 0u) {
            out[code_at] = code;
            code_at = o++;
            code = 1;
        } else {
            out[o++] = in[i];
            if (++code == 0xFFu) {
                out[code_at] = code;
                code_at = o++;
                code = 1;
            }
        }
    }
    out[code_at] = code;
    return o;
}

size_t rl_cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t max)
{
    size_t i = 0, o = 0;

    if (n == 0u)
        return RL_COBS_ERROR;
    while (i < n) {
        uint8_t code = in[i++];
        unsigned k;
        if (code == 0u || (size_t)code - 1u > n - i)
            return RL_COBS_ERROR;
        for (k = 1; k < code; k++) {
            if (in[i] == 0u || o >= max)
                return RL_COBS_ERROR;
            out[o++] = in[i++];
        }
        if (code != 0xFFu && i < n) {
            if (o >= max)
                return RL_COBS_ERROR;
            out[o++] = 0u;
        }
    }
    return o;
}

size_t rl_frame_encode(uint8_t type, uint8_t seq, const uint8_t *payload, size_t len,
                       uint8_t *out, size_t out_size)
{
    uint8_t f[RL_FRAME_MAX];
    uint16_t crc;
    size_t i, n;

    if (len > RL_MAX_PAYLOAD || out_size < RL_WIRE_SIZE(len))
        return 0;
    f[0] = type;
    f[1] = seq;
    f[2] = (uint8_t)len;
    f[3] = (uint8_t)(len >> 8);
    for (i = 0; i < len; i++)
        f[RL_HDR_LEN + i] = payload[i];
    crc = rl_crc16(f, RL_HDR_LEN + len, RL_CRC_INIT);
    f[RL_HDR_LEN + len] = (uint8_t)crc;
    f[RL_HDR_LEN + len + 1u] = (uint8_t)(crc >> 8);
    n = rl_cobs_encode(f, RL_HDR_LEN + len + RL_CRC_LEN, out);
    out[n++] = 0u;
    return n;
}

rl_frame_status_t rl_frame_parse(const uint8_t *f, size_t n, rl_frame_t *out)
{
    uint16_t len, crc;

    if (n < RL_HDR_LEN + RL_CRC_LEN)
        return RL_FRAME_SHORT;
    len = (uint16_t)(f[2] | ((uint16_t)f[3] << 8));
    if (len > RL_MAX_PAYLOAD || (size_t)len + RL_HDR_LEN + RL_CRC_LEN != n)
        return RL_FRAME_LEN;
    crc = (uint16_t)(f[n - 2u] | ((uint16_t)f[n - 1u] << 8));
    if (rl_crc16(f, n - RL_CRC_LEN, RL_CRC_INIT) != crc)
        return RL_FRAME_CRC;
    out->type = f[0];
    out->seq = f[1];
    out->len = len;
    out->payload = f + RL_HDR_LEN;
    return RL_FRAME_OK;
}
