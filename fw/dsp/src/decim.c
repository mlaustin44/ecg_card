#include "dsp_decim.h"

#include "dsp_coefs.h"
#include "dsp_util.h"

#if DSP_DECIM_L != DSP_DECIM_BRANCH
#error "dsp_decim.h DSP_DECIM_BRANCH does not match the generated coefficients"
#endif

static const int16_t poly[8][DSP_DECIM_L] = DSP_DECIM_POLY_INIT;
static const int32_t tail[DSP_DECIM_L] = DSP_DECIM_TAIL_INIT;

void dsp_decim_init(dsp_decim_t *d, uint16_t mid_code)
{
    for (unsigned j = 0; j < DSP_DECIM_L; j++)
        d->acc[j] = 0;
    d->mid = (int16_t)mid_code;
    d->phase = 0;
    d->head = 0;
    d->primed = 0;
}

size_t dsp_decim_push(dsp_decim_t *d, const uint16_t *raw, size_t n, int16_t *out)
{
    size_t nout = 0;
    if (n == 0)
        return 0;
    if (!d->primed) {
        /* as if every earlier input had equalled the first one (head and
         * phase are 0 here: priming only follows dsp_decim_init) */
        int32_t v = (int32_t)raw[0] - d->mid;
        for (unsigned j = 0; j < DSP_DECIM_L; j++)
            d->acc[j] = v * tail[j];
        d->primed = 1;
    }
    unsigned head = d->head;
    unsigned phase = d->phase;
    int32_t *acc = d->acc;
    for (size_t i = 0; i < n; i++) {
        int32_t x = (int32_t)raw[i] - d->mid;
        const int16_t *h = poly[phase];
        /* acc[(head + j) % L] += h[j] * x, split to avoid the modulo */
        unsigned j = 0;
        unsigned k = head;
        for (; k < DSP_DECIM_L; j++, k++)
            acc[k] += h[j] * x;
        for (k = 0; j < DSP_DECIM_L; j++, k++)
            acc[k] += h[j] * x;
        if (++phase == 8u) {
            phase = 0;
            /* y = acc * 8 / 32768, rounded */
            out[nout++] = dsp_sat16(dsp_rshr32(acc[head], 12));
            acc[head] = 0;
            if (++head == DSP_DECIM_L)
                head = 0;
        }
    }
    d->head = (uint8_t)head;
    d->phase = (uint8_t)phase;
    return nout;
}
