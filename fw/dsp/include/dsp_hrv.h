/* Session statistics: beat count, heart rate avg/min/max, SDNN, RMSSD.
 *
 * Incremental, bounded memory (64 bytes). Feed every detected beat with its
 * R-R interval in ms (dsp_qrs_beat_t.rr_ms; 0 for the first beat after init
 * or re-contact).
 *
 * Outlier rule (ectopic / missed / extra beats): an interval is a normal-to-
 * normal (NN) interval if 273 <= RR <= 2000 ms (220..30 bpm) and, once 3
 * intervals have been seen, it is within +-20 % of the median of the last
 * (up to 5) raw intervals, accepted or not, so genuine rate changes are
 * followed. Rejected intervals are excluded from all statistics, and the
 * successive difference for RMSSD is only formed between two consecutive
 * accepted intervals (a rejected beat breaks the chain).
 *   HR avg = 60000 / mean(NN);  HR min/max = extremes of the median-of-5 NN
 *   rate (the displayed rate, not single beats);
 *   SDNN = sample standard deviation of NN;  RMSSD = rms of successive NN
 *   differences. All rounded to integers; 0 = not available (SDNN needs 2
 *   NN, RMSSD 1 difference, HR min/max 5 NN). */
#ifndef DSP_HRV_H
#define DSP_HRV_H

#include <stdint.h>

typedef struct {
    uint64_t sum_d2;   /* sum (nn - ref)^2, ms^2 */
    uint64_t sum_sd2;  /* sum of successive differences squared, ms^2 */
    uint32_t sum_nn;   /* ms */
    int32_t sum_d;     /* sum (nn - ref), ms */
    uint16_t beats;    /* saturating */
    uint16_t n_nn, n_sd;
    uint16_t ref;      /* first NN, centres the variance sums */
    uint16_t prev_nn;  /* previous interval if it was accepted, else 0 */
    uint16_t raw[5];   /* last raw intervals (outlier reference) */
    uint16_t nn5[5];   /* last accepted intervals (HR min / max) */
    uint16_t hr_min, hr_max;
    uint8_t raw_n, raw_i, nn5_n, nn5_i;
} dsp_hrv_t;

typedef struct {
    uint16_t beats;
    uint16_t nn_count;
    uint16_t hr_avg, hr_min, hr_max; /* bpm */
    uint16_t sdnn_ms, rmssd_ms;
} dsp_hrv_summary_t;

void dsp_hrv_init(dsp_hrv_t *h);

/* One detected beat. rr_ms = interval from the previous beat, 0 if none
 * (first beat; also breaks the RMSSD chain and the outlier reference).
 * Returns 1 if the interval was accepted as NN, else 0. */
int dsp_hrv_add_beat(dsp_hrv_t *h, uint16_t rr_ms);

void dsp_hrv_summary(const dsp_hrv_t *h, dsp_hrv_summary_t *out);

#endif
