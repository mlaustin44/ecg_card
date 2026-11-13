#include "dsp_ecg_disp.h"

#include "dsp_coefs.h"
#include "dsp_util.h"

static const dsp_bq_coef_t c_hp = DSP_DISP_HP_INIT;
static const dsp_bq_coef_t c_n50 = DSP_DISP_NOTCH50_INIT;
static const dsp_bq_coef_t c_n60 = DSP_DISP_NOTCH60_INIT;
static const dsp_bq_coef_t c_lp = DSP_DISP_LP_INIT;

int32_t dsp_uv_q20(int gain); /* mains.c */

void dsp_disp_init(dsp_disp_t *s, int gain, uint8_t notch_hz)
{
    s->uv_q20 = dsp_uv_q20(gain);
    dsp_disp_set_notch(s, notch_hz);
    dsp_disp_reset(s);
}

void dsp_disp_set_notch(dsp_disp_t *s, uint8_t notch_hz)
{
    s->notch_hz = (notch_hz == 50u || notch_hz == 60u) ? notch_hz : 0u;
}

void dsp_disp_reset(dsp_disp_t *s)
{
    s->primed = 0;
}

void dsp_disp_process(dsp_disp_t *s, const int16_t *in, int16_t *out_uv, size_t n)
{
    const dsp_bq_coef_t *cn = s->notch_hz == 60u ? &c_n60 : &c_n50;
    for (size_t i = 0; i < n; i++) {
        int32_t x = (int32_t)in[i] * 256; /* 8 guard bits */
        if (!s->primed) {
            dsp_bq_reset(&s->hp, x, 0);
            dsp_bq_reset(&s->notch, 0, 0);
            dsp_bq_reset(&s->lp, 0, 0);
            s->primed = 1;
        }
        int32_t y = dsp_bq_step(&s->hp, &c_hp, x);
        if (s->notch_hz)
            y = dsp_bq_step(&s->notch, cn, y);
        y = dsp_bq_step(&s->lp, &c_lp, y);
        /* uV = y / 256 * uv_q20 / 2^20 */
        out_uv[i] = dsp_sat16_64(dsp_rshr64((int64_t)y * s->uv_q20, 28));
    }
}
