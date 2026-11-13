/* Fixed-point biquad used by every IIR filter in ecg_dsp.
 *
 * Direct form I, Q30 coefficients, int32 signal and state, 64-bit products,
 * with fraction saving (first-order error feedback): the bits discarded by
 * the >> 30 are added back on the next sample. This makes the quantisation
 * noise transfer (1 - z^-1), removes the dead band and zero-input limit
 * cycles of poles near z = 1 (0.5 Hz high-passes) and keeps DC exact.
 *
 * Signals inside ecg_dsp are int32 with fractional guard bits (see each
 * module); headroom is the caller module's responsibility. */
#ifndef DSP_BIQUAD_H
#define DSP_BIQUAD_H

#include <stdint.h>

/* y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2, all coefficients Q30. */
typedef struct {
    int32_t b0, b1, b2, a1, a2;
} dsp_bq_coef_t;

typedef struct {
    int32_t x1, x2, y1, y2;
    uint32_t e; /* saved fraction, 0 .. 2^30 - 1 */
} dsp_bq_t;     /* 20 bytes */

/* Set the state as if input x0 and output y0 had been constant forever. */
void dsp_bq_reset(dsp_bq_t *s, int32_t x0, int32_t y0);

/* One sample. */
int32_t dsp_bq_step(dsp_bq_t *s, const dsp_bq_coef_t *c, int32_t x);

#endif
