/* ECG display path at 500 Hz: stream units in, microvolts out.
 *
 *   1st-order 0.5 Hz high-pass  ->  mains notch (50 / 60 Hz, Q 30, or off)
 *   ->  2nd-order Butterworth 40 Hz low-pass  ->  x 805.7 / (8 * gain)
 *
 * Internal precision: stream units with 8 fractional bits, Q30 biquads with
 * fraction saving (dsp_biquad.h). Notch envelope time constant 190 ms (50 Hz)
 * / 160 ms (60 Hz); HP time constant 318 ms. Group delay at 10 Hz is
 * DSP_DISP_DELAY_Q8 / 256 samples (about 6.8 ms): for drawing only. */
#ifndef DSP_ECG_DISP_H
#define DSP_ECG_DISP_H

#include <stddef.h>
#include <stdint.h>

#include "dsp_biquad.h"

typedef struct {
    dsp_bq_t hp, notch, lp;
    int32_t uv_q20;   /* microvolts per stream unit, Q20 */
    uint8_t notch_hz; /* 0 (off), 50 or 60 */
    uint8_t primed;
} dsp_disp_t;         /* 68 bytes */

/* gain: ECG_GAIN_LO (558) or ECG_GAIN_HI (1098). notch_hz: 50, 60 or 0 (off). */
void dsp_disp_init(dsp_disp_t *s, int gain, uint8_t notch_hz);

/* Change the notch (keeps the filter running; expect a ~0.5 s transient). */
void dsp_disp_set_notch(dsp_disp_t *s, uint8_t notch_hz);

/* Restart from the next sample with no step transient (re-contact). */
void dsp_disp_reset(dsp_disp_t *s);

/* n stream samples in -> n microvolt samples out (saturated to int16). */
void dsp_disp_process(dsp_disp_t *s, const int16_t *in, int16_t *out_uv, size_t n);

#endif
