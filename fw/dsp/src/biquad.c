#include "dsp_biquad.h"

void dsp_bq_reset(dsp_bq_t *s, int32_t x0, int32_t y0)
{
    s->x1 = s->x2 = x0;
    s->y1 = s->y2 = y0;
    s->e = 0;
}

int32_t dsp_bq_step(dsp_bq_t *s, const dsp_bq_coef_t *c, int32_t x)
{
    int64_t acc = (int64_t)c->b0 * x + (int64_t)c->b1 * s->x1 + (int64_t)c->b2 * s->x2
                - (int64_t)c->a1 * s->y1 - (int64_t)c->a2 * s->y2 + (int64_t)s->e;
    int32_t y = (int32_t)(acc >> 30);              /* floor */
    s->e = (uint32_t)((uint64_t)acc & 0x3FFFFFFFu); /* fraction saving */
    s->x2 = s->x1;
    s->x1 = x;
    s->y2 = s->y1;
    s->y1 = y;
    return y;
}
