#include "stream.h"

#include "hal.h"
#include "stream_fmt.h"

static uint8_t s_seq;
static uint32_t s_frames, s_drops;

uint16_t crc16_ccitt_false(const uint8_t *p, size_t n, uint16_t crc)
{
    while (n--) {
        crc ^= (uint16_t)((uint16_t)*p++ << 8);
        for (unsigned b = 0; b < 8u; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

size_t cobs_encode(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t o = 1, code_at = 0;
    uint8_t code = 1;
    for (size_t i = 0; i < n; i++) {
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

size_t cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t max)
{
    size_t i = 0, o = 0;
    if (n == 0)
        return (size_t)-1;
    while (i < n) {
        uint8_t code = in[i++];
        if (code == 0u || i + code - 1u > n)
            return (size_t)-1;
        for (unsigned k = 1; k < code; k++) {
            if (in[i] == 0u || o >= max)
                return (size_t)-1;
            out[o++] = in[i++];
        }
        if (code != 0xFFu && i < n) {
            if (o >= max)
                return (size_t)-1;
            out[o++] = 0u;
        }
    }
    return o;
}

size_t stream_build(uint8_t type, uint8_t seq, const uint8_t *payload, size_t len,
                    uint8_t out[COBS_MAX(STREAM_FRAME_MAX) + 1u])
{
    uint8_t f[STREAM_FRAME_MAX];
    if (len > STREAM_APP_MAX_PAYLOAD)
        return 0;
    f[0] = type;
    f[1] = seq;
    f[2] = (uint8_t)len;
    f[3] = (uint8_t)(len >> 8);
    for (size_t i = 0; i < len; i++)
        f[4 + i] = payload[i];
    uint16_t crc = crc16_ccitt_false(f, 4u + len, 0xFFFFu);
    f[4 + len] = (uint8_t)crc;
    f[5 + len] = (uint8_t)(crc >> 8);
    size_t n = cobs_encode(f, len + 6u, out);
    out[n++] = 0u;
    return n;
}

void stream_init(void)
{
    s_seq = 0;
    s_frames = s_drops = 0;
}

bool stream_send(uint8_t type, const uint8_t *payload, size_t len)
{
    static uint8_t wire[COBS_MAX(STREAM_FRAME_MAX) + 1u];
    size_t n = stream_build(type, s_seq, payload, len, wire);
    if (n == 0)
        return false;
    /* The sequence advances for every frame produced, so the receiver
     * counts frames dropped here as gaps. */
    s_seq++;
    if (hal_stream_write(wire, n) != n) {
        s_drops++;
        return false;
    }
    s_frames++;
    return true;
}

uint32_t stream_frames(void) { return s_frames; }
uint32_t stream_drops(void) { return s_drops; }

static uint8_t *put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return p + 2;
}
static uint8_t *put32(uint8_t *p, uint32_t v)
{
    p = put16(p, (uint16_t)v);
    return put16(p, (uint16_t)(v >> 16));
}

void stream_ecg_block(uint32_t start, uint8_t flags, const int16_t *s)
{
    uint8_t b[4 + 1 + 2 * STREAM_ECG_SAMPLES_PER_BLOCK], *p = put32(b, start);
    *p++ = flags;
    for (unsigned i = 0; i < STREAM_ECG_SAMPLES_PER_BLOCK; i++)
        p = put16(p, (uint16_t)s[i]);
    stream_send(STREAM_ECG_BLOCK, b, sizeof b);
}

void stream_ppg_block(uint32_t start, uint16_t led_code, uint8_t flags, const int16_t *cf)
{
    uint8_t b[4 + 2 + 1 + 4 * STREAM_PPG_FRAMES_PER_BLOCK], *p = put32(b, start);
    p = put16(p, led_code);
    *p++ = flags;
    for (unsigned i = 0; i < 2u * STREAM_PPG_FRAMES_PER_BLOCK; i++)
        p = put16(p, (uint16_t)cf[i]);
    stream_send(STREAM_PPG_BLOCK, b, sizeof b);
}

void stream_ev_r_peak(uint32_t index, uint16_t hr_bpm)
{
    uint8_t b[7], *p = b;
    *p++ = STREAM_EV_R_PEAK;
    p = put32(p, index);
    put16(p, hr_bpm);
    stream_send(STREAM_EVENT, b, sizeof b);
}

void stream_ev_pulse(uint32_t foot_index, uint16_t pulse_bpm, uint16_t pat_ms, uint16_t pi)
{
    uint8_t b[11], *p = b;
    *p++ = STREAM_EV_PULSE;
    p = put32(p, foot_index);
    p = put16(p, pulse_bpm);
    p = put16(p, pat_ms);
    put16(p, pi);
    stream_send(STREAM_EVENT, b, sizeof b);
}

void stream_ev_lead(bool contact)
{
    uint8_t b[2] = {STREAM_EV_LEAD, contact ? 1u : 0u};
    stream_send(STREAM_EVENT, b, sizeof b);
}

void stream_ev_state(uint8_t mode, uint8_t notch_hz, uint16_t mid_code, uint8_t batt_pct,
                     uint8_t flags)
{
    uint8_t b[7], *p = b;
    *p++ = STREAM_EV_STATE;
    *p++ = mode;
    *p++ = notch_hz;
    p = put16(p, mid_code);
    *p++ = batt_pct;
    *p = flags;
    stream_send(STREAM_EVENT, b, sizeof b);
}

void stream_ev_button(uint8_t button, uint8_t kind)
{
    uint8_t b[3] = {STREAM_EV_BUTTON, button, kind};
    stream_send(STREAM_EVENT, b, sizeof b);
}
