/* PPG processing at 100 Hz: frame reconstruction, bridging, band-pass,
 * pulse detection, intersecting-tangent foot, pulse rate, perfusion index.
 *
 * Units and sign convention
 *   coarse = tia_on - tia_dark, ADC codes (0.806 nA photocurrent per code).
 *   fine   = -(photocurrent) in units of TIA_OUT / 10 codes (80.6 pA):
 *            from PPG_HI = 11 V_DAC12 - 10 TIA_OUT (all in ADC codes, VDDA
 *            reference for ADC and DAC12):
 *              fine = hi_on - 11 dac12 + dark10
 *            dark10 = 11 dac12 - hi_dark if hi_dark is in range (then fine =
 *            hi_on - hi_dark exactly), else 10 tia_dark. If hi_on itself is on
 *            a rail, fine falls back to -10 coarse (same units, coarser).
 *            Because the DAC12 code enters the formula, fine is continuous
 *            across DCC offset steps by construction.
 *            Sign: more blood -> less reflected light -> fine goes UP, so
 *            fine and the display trace show the systolic upstroke upward
 *            (conventional plethysmogram). The stream sends fine as int16
 *            (wraps modulo 2^16; the absolute value is -light).
 * Signal path (processing; the stream gets the per-frame fine above)
 *   Because hi_dark usually rails, the dark term is 10 x tia_dark, whose ADC
 *   noise x10 dominates. Processing therefore uses 10 x the centred 15-frame
 *   mean of tia_dark (aliased 100/120 Hz flicker below 0.5 Hz still cancels
 *   to > 99 %; 120 Hz flicker aliases to 20 Hz and is removed by the LP; dark
 *   samples do not depend on the DAC12/LED codes, so the mean may span
 *   disturbed frames). This delays processing by 7 frames (removed again).
 *   fine -> bridge -> 21-tap linear-phase FIR LP (-3 dB 11.4 Hz, delay 10
 *   frames) -> [pulse detection, foot]
 *                   -> 1st-order 0.5 Hz HP -> Butterworth-2 8 Hz LP -> disp
 *   (display band 0.48-7.98 Hz). The foot path is kept wide (11 Hz) because
 *   smoothing the upstroke biases the tangent foot early (-3.8 ms at 7.5 Hz,
 *   -0.5 ms at 11.4 Hz on the test pulse shape).
 * Bridging: frames flagged with any DSP_PPG_IN_* flag are not used; the
 *   bridged signal is extrapolated linearly from the last two good frames
 *   (held flat after 3 frames). After an LED step (or a hi/coarse source
 *   switch) the first good frame is re-offset onto the extrapolation, so the
 *   light-level step never reaches the filters. DAC12 steps need no
 *   re-offset (formula continuity). Pulses whose analysis window overlaps a
 *   disturbed frame are not reported.
 * Pulse detection: slope of the display-band signal (central difference)
 *   against an adaptive threshold (0.4 x running mean of accepted maximum
 *   slopes, learned over the first 2 s, halved every second after 3 s
 *   without a pulse), 280 ms refractory. A candidate is rejected if its rise
 *   (minimum to tangent point) is < 1/4 of the running mean rise.
 * Foot (intersecting tangents): tangent at the maximum upslope (parabolic
 *   sub-sample location and slope) intersected with the horizontal through
 *   the minimum preceding it (parabolic minimum value, searched back to the
 *   previous pulse, at most 450 ms). The FIR delay is removed, so the foot is
 *   on the input frame timeline: frame index + frac / 256, where frame k is
 *   the k-th frame passed after dsp_ppg_init() (its on-sample is at
 *   k * 10 ms + 9.95 ms).
 * Pulse rate: median of the last 5 foot-to-foot intervals (0 until 6 feet).
 *   Intervals are only formed between consecutive accepted pulses (none
 *   across a rejected / disturbed one) and skipped if outside 0.3..2.5 s or
 *   >= 1.6 x the current median (a missed pulse).
 * Perfusion index: AC / DC in 0.1 % units, reported one beat late: AC =
 *   the previous beat's LP maximum minus that beat's own foot minimum (fine
 *   units), DC = 10 x coarse (EMA, tau 320 ms) => PI = AC * 100 / coarse.
 *   0 for the first pulse and after a rejected one. */
#ifndef DSP_PPG_H
#define DSP_PPG_H

#include <stdbool.h>
#include <stdint.h>

#include "dsp_biquad.h"

#define DSP_PPG_FIR_TAPS 21u
#define DSP_PPG_RING     64u   /* LP history, frames (power of 2) */
#define DSP_PPG_DARK_N   15u   /* centred tia_dark mean (processing only) */
#define DSP_PPG_HIST     8u    /* delayed-frame history (power of 2, > delay) */

/* ADC codes of one frame (12-bit). */
typedef struct {
    uint16_t tia_dark, hi_dark, tia_on, hi_on;
} dsp_ppg_frame_t;

/* Input flags: what happened before (or during) this frame. */
#define DSP_PPG_IN_DAC_STEP  0x01u /* DAC12 offset changed */
#define DSP_PPG_IN_LED_STEP  0x02u /* LED current changed */
#define DSP_PPG_IN_DISTURBED 0x04u /* other: bridge, no re-offset */

/* Output flags. */
#define DSP_PPG_OUT_BRIDGED 0x01u /* frame was bridged (not measured) */
#define DSP_PPG_OUT_COARSE  0x02u /* hi_on on a rail: fine from coarse */
#define DSP_PPG_OUT_PULSE   0x04u /* *pulse was filled */

typedef struct {
    int16_t coarse; /* stream "coarse" */
    int16_t fine;   /* stream "fine" (raw, wraps) */
    int16_t disp;   /* band-passed pleth for UI_TRACE_PPG (fine units) */
    uint8_t flags;  /* DSP_PPG_OUT_* */
} dsp_ppg_out_t;

typedef struct {
    uint32_t frame;      /* foot: input frame index ...           */
    uint8_t frac_q8;     /* ... + frac_q8 / 256 frame             */
    uint16_t ibi_ms;     /* foot-to-foot interval, 0 for the first */
    uint16_t pulse_bpm;  /* median of 5, 0 = n/a */
    uint16_t pi_permille;/* 0 = n/a */
} dsp_ppg_pulse_t;

typedef struct {
    int32_t fir[DSP_PPG_FIR_TAPS]; /* bridged input history */
    int32_t s[DSP_PPG_RING];       /* LP output history */
    dsp_bq_t hp, lp;               /* display only */
    uint32_t t;                    /* frames processed */
    int32_t b1, b2;                /* last two bridged values */
    int32_t offset;
    int32_t thr, slope_avg, learn_max, up_max;
    int32_t y1, y2;                /* display output history (detection) */
    int32_t amp_avg;               /* running mean upstroke rise */
    int32_t smax, prev_min;        /* running max, previous foot level */
    int32_t dc_q5;                 /* coarse EMA, 5 fractional bits */
    uint32_t up_t, last_up_t;
    uint32_t last_foot_f;
    uint32_t ibi_q8[5];
    uint32_t sum_dark, sum_tia, sum_hi; /* for dsp_ppg_take_avg() */
    int32_t dpart[DSP_PPG_HIST];   /* frames awaiting the centred dark mean */
    int32_t dexact[DSP_PPG_HIST];
    uint32_t dark_sum;
    uint16_t dark[DSP_PPG_DARK_N];
    uint8_t dflags[DSP_PPG_HIST];
    uint8_t dk_i, dh_i;
    uint16_t n_avg;
    uint16_t dist_age;             /* frames since the last disturbed frame */
    uint16_t no_pulse;             /* frames since the last pulse */
    uint8_t last_foot_frac;
    uint8_t n_bridge;
    uint8_t ibi_n, ibi_i;
    uint8_t fir_i;
    uint8_t flags;                 /* internal state bits */
} dsp_ppg_t;                       /* 600 bytes */

/* Reconstruct one frame (pure). *coarse_mode set if hi_on is on a rail. */
int32_t dsp_ppg_fine(const dsp_ppg_frame_t *f, uint16_t dac12, bool *coarse_mode);

void dsp_ppg_init(dsp_ppg_t *p);

/* Re-contact: forget detection state and re-prime the filters on the next
 * frame; the frame counter keeps counting. */
void dsp_ppg_reset(dsp_ppg_t *p);

/* Process one frame. dac12 = DAC12 code in effect during the frame,
 * in_flags = DSP_PPG_IN_*. Returns true and fills *pulse when a pulse foot
 * was found (reported about 0.2-0.35 s after the foot). */
bool dsp_ppg_process(dsp_ppg_t *p, const dsp_ppg_frame_t *f, uint16_t dac12,
                     uint8_t in_flags, dsp_ppg_out_t *out, dsp_ppg_pulse_t *pulse);

/* Mean codes over the undisturbed frames since the previous call (inputs of
 * the control laws, dsp_ppg_ctrl.h). Returns the frame count (0: *avg
 * untouched). */
typedef struct {
    uint16_t tia_dark, tia_on, hi_on;
} dsp_ppg_avg_t;
uint16_t dsp_ppg_take_avg(dsp_ppg_t *p, dsp_ppg_avg_t *avg);

#endif
