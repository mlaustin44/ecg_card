#include "dsp_mains.h"

#include "dsp_coefs.h"
#include "dsp_util.h"

int32_t dsp_uv_q20(int gain);

int32_t dsp_uv_q20(int gain)
{
    return gain >= 1000 ? DSP_UV_Q20_G1098 : DSP_UV_Q20_G558;
}

static const int32_t coef[2] = { DSP_MAINS_C50, DSP_MAINS_C60 };

void dsp_mains_init(dsp_mains_t *m, int gain)
{
    m->s1[0] = m->s1[1] = m->s2[0] = m->s2[1] = 0;
    m->pp_q28 = gain >= 1000 ? DSP_MAINS_PP_Q28_G1098 : DSP_MAINS_PP_Q28_G558;
    m->n = 0;
    m->uvpp[0] = m->uvpp[1] = 0;
    m->sel_hz = 0;
    m->cand_hz = 0;
    m->cand_cnt = 0;
    m->windows = 0;
}

static void decide(dsp_mains_t *m)
{
    uint16_t a50 = m->uvpp[0], a60 = m->uvpp[1];
    uint8_t dom = 0;
    if (a50 >= DSP_MAINS_FLOOR_UVPP && a50 >= 2u * (uint32_t)a60)
        dom = 50;
    else if (a60 >= DSP_MAINS_FLOOR_UVPP && a60 >= 2u * (uint32_t)a50)
        dom = 60;
    if (dom == 0 || dom == m->sel_hz) {
        m->cand_cnt = 0;
        return;
    }
    if (m->sel_hz == 0) {
        m->sel_hz = dom;
        m->cand_cnt = 0;
        return;
    }
    if (dom == m->cand_hz)
        m->cand_cnt++;
    else {
        m->cand_hz = dom;
        m->cand_cnt = 1;
    }
    if (m->cand_cnt >= 2u) {
        m->sel_hz = dom;
        m->cand_cnt = 0;
    }
}

bool dsp_mains_push(dsp_mains_t *m, const int16_t *x, size_t n)
{
    bool done = false;
    for (size_t i = 0; i < n; i++) {
        for (unsigned b = 0; b < 2; b++) {
            int32_t s0 = (int32_t)x[i] + (int32_t)(((int64_t)coef[b] * m->s1[b]) >> 30) - m->s2[b];
            m->s2[b] = m->s1[b];
            m->s1[b] = s0;
        }
        if (++m->n == DSP_MAINS_N) {
            for (unsigned b = 0; b < 2; b++) {
                int64_t s1 = m->s1[b], s2 = m->s2[b];
                int64_t p = s1 * s1 + s2 * s2 - ((coef[b] * s1) >> 30) * s2;
                uint32_t mag = dsp_isqrt64(p > 0 ? (uint64_t)p : 0u);
                int64_t uv = dsp_rshr64((int64_t)mag * m->pp_q28, 28);
                m->uvpp[b] = uv > 65535 ? 65535u : (uint16_t)uv;
                m->s1[b] = m->s2[b] = 0;
            }
            m->n = 0;
            if (m->windows < 255u)
                m->windows++;
            decide(m);
            done = true;
        }
    }
    return done;
}

uint16_t dsp_mains_uvpp(const dsp_mains_t *m, uint8_t hz)
{
    return hz == 60u ? m->uvpp[1] : m->uvpp[0];
}

uint16_t dsp_mains_residual_uvpp(const dsp_mains_t *m)
{
    return m->uvpp[0] > m->uvpp[1] ? m->uvpp[0] : m->uvpp[1];
}

uint8_t dsp_mains_auto_hz(const dsp_mains_t *m)
{
    return m->sel_hz;
}
