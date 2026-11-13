/* Pulse arrival time: ECG R-peak -> PPG pulse foot.
 *
 * Shared timeline (us, modulo 2^32): ECG raw sample n at n * 250 us, PPG
 * frame k's on-sample at k * 10 ms + 9.95 ms, both from the common start
 * event. Every filter delay is removed:
 *   t_R    = index * 2000 + frac * 2000/256 + DSP_ECG_STREAM_T0_US
 *            - DSP_ECG_ANALOG_DELAY_US      (decimator and analog chain; the
 *            QRS clean-LP delay is already out of the index)
 *   t_foot = frame * 10000 + frac * 10000/256 + PPG_T_ON_SAMPLE_US
 *            - DSP_PPG_ANALOG_DELAY_US      (FIR delay already out of frame)
 * Pairing: each foot is paired with the latest R-peak before it, once an
 * R-peak after the foot has been seen (so a late-reported R cannot be
 * skipped). Accepted if 100 <= PAT <= 500 ms and the R-R interval that
 * contains the foot is <= 1.5 x the previous one (rejects a foot paired
 * across a missed R). Feed R-peaks and feet in their own order; read results
 * with dsp_pat_pop(). Call dsp_pat_init() on re-contact. */
#ifndef DSP_PAT_H
#define DSP_PAT_H

#include <stdbool.h>
#include <stdint.h>

#define DSP_PAT_MIN_US 100000
#define DSP_PAT_MAX_US 500000

typedef struct {
    uint32_t r_us[4];    /* last R-peak times, ring */
    uint32_t foot_us[4]; /* pending feet, FIFO */
    uint32_t out_us[4];  /* results, FIFO */
    uint32_t out_foot[4];
    uint8_t r_n, r_i, f_n, f_i, o_n, o_i;
    uint16_t rejected;
} dsp_pat_t;             /* 72 bytes */

void dsp_pat_init(dsp_pat_t *p);

/* R-peak from dsp_qrs (stream index + frac). */
void dsp_pat_add_r(dsp_pat_t *p, uint32_t index, uint8_t frac_q8);

/* Foot from dsp_ppg (input frame index + frac). */
void dsp_pat_add_foot(dsp_pat_t *p, uint32_t frame, uint8_t frac_q8);

/* Next PAT result in us, and the foot time it belongs to (shared timeline,
 * may be NULL); false if none. */
bool dsp_pat_pop(dsp_pat_t *p, uint32_t *pat_us, uint32_t *foot_us);

/* Timeline helpers (us, modulo 2^32). */
uint32_t dsp_pat_r_time_us(uint32_t index, uint8_t frac_q8);
uint32_t dsp_pat_foot_time_us(uint32_t frame, uint8_t frac_q8);

/* Foot time as a 500 Hz stream index (STREAM_EV_PULSE "foot time in ECG
 * sample units"): the stream sample that represents the same instant. */
uint32_t dsp_pat_foot_ecg_index(uint32_t frame, uint8_t frac_q8);

#endif
