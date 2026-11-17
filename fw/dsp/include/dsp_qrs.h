/* QRS detection (Pan-Tompkins) and heart rate on the 500 Hz stream.
 *
 * Signal path (all at 500 Hz, int32 with 8 fractional bits):
 *   stream -> clean LP (Bessel-4 poles, exact nulls at 50 and 60 Hz,
 *             -3 dB 18 Hz, flat group delay 5.57 samples)       [R refinement]
 *          -> band-pass 5-15 Hz (Butterworth-2 HP + LP)
 *          -> derivative 2x[n] + x[n-2] - x[n-6] - 2x[n-8]
 *          -> squaring -> 150 ms moving-window integral (MWI)
 * Decision logic (Pan & Tompkins 1985):
 *   - MWI local maxima are candidates once no larger value follows for 80 ms;
 *   - dual adaptive thresholds on the MWI peak (SPKI/NPKI) and on the band-
 *     pass peak (SPKF/NPKF): THR1 = N + (S - N) / 4, THR2 = THR1 / 2;
 *     learning phase = 2.05 s after a 0.2 s settling time following init /
 *     reset (S = max/3, N = mean/2); the last learning candidate >= half the
 *     largest becomes the previous beat (reported if < 1.5 s old), so the
 *     refractory and T-wave tests work from the first classified candidate;
 *   - 200 ms refractory (candidates whose refined R lies < 200 ms after the
 *     previous beat are discarded);
 *   - T-wave discrimination: a candidate < 360 ms after the previous beat
 *     whose maximum slope is < 1/2 of that beat's is a noise peak;
 *   - search-back: if no beat is found within 1.66 x the mean of the last 8
 *     RR intervals, the largest candidate above THR2 since the last beat is
 *     accepted (S updated with weight 1/4) and flagged DSP_QRS_F_SEARCHBACK;
 *   - after 2 s without a beat S and N halve every second (amplitude drops).
 * R-peak refinement: the band-pass maximum locates the QRS; the R-peak is the
 * extremum of the clean signal within +-50 ms of it (polarity learned from a
 * running vote, positive R by default), parabolically interpolated, minus the
 * clean LP delay. Reported indices are therefore on the stream timeline:
 * index 0 is the first sample passed after dsp_qrs_init().
 *
 * Latency: a beat is reported 80 ms after its MWI peak, i.e. typically
 * 190-260 ms after the R-peak (DSP_QRS_LATENCY_MAX upper bound for normal
 * beats); search-back beats are reported when the miss is noticed, up to
 * 1.66 RR + 300 ms after the R-peak.
 *
 * Heart rate: median of the last 5 R-R intervals (0 until 6 beats).
 * RAM: 1248 bytes per detector (rings of 256 + 2 x 128 int16). */
#ifndef DSP_QRS_H
#define DSP_QRS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp_biquad.h"

#define DSP_QRS_RING  256u  /* clean-signal history, samples (power of 2) */
#define DSP_QRS_DRING 128u  /* band-pass / derivative history (power of 2) */
#define DSP_QRS_MWI   75u   /* 150 ms integration window */
#define DSP_QRS_LATENCY_MAX 150u /* samples (300 ms), R-peak -> report, normal beats */

#define DSP_QRS_F_SEARCHBACK 0x01u

typedef struct {
    uint32_t index;   /* R-peak sample index, stream timeline (500 Hz) */
    uint8_t frac_q8;  /* + frac_q8 / 256 sample */
    uint8_t flags;    /* DSP_QRS_F_* */
    uint16_t rr_ms;   /* interval from the previous beat, 0 for the first */
    uint16_t hr_bpm;  /* median-of-5 heart rate after this beat, 0 = n/a */
    uint16_t amp;     /* QRS peak-to-peak in the clean signal, stream units */
} dsp_qrs_beat_t;

typedef struct {
    dsp_bq_t clean[2], hp, lp;
    uint32_t t;                    /* index of the next input sample */
    int16_t cr[DSP_QRS_RING];      /* clean signal, stream units */
    int16_t br[DSP_QRS_DRING];     /* band-pass, stream units */
    int16_t dr[DSP_QRS_DRING];     /* derivative, 2x slope per 2 ms */
    uint32_t mwi;                  /* sum of dr^2 over the last 75 samples */
    uint32_t cand_v, cand_t;       /* MWI peak being tracked / valley */
    uint32_t spki, npki, spkf, npkf;
    uint32_t thr_i1, thr_f1;
    uint64_t learn_sum_i;
    uint32_t learn_max_i, learn_sum_f, learn_max_f;
    uint32_t last_r;               /* last beat: R index, frac, max slope */
    uint32_t last_slope;
    uint32_t last_beat_t;          /* t when the last beat was accepted */
    uint32_t decay_t;              /* next threshold decay */
    uint32_t sb_peaki, sb_peakf, sb_r, sb_slope;
    uint32_t rr_q8[5];             /* last 5 RR, 1/256 samples */
    uint16_t rr8[8];               /* last 8 RR, samples (search-back mean) */
    uint16_t sb_amp, learn_n;
    uint16_t sb_lim;               /* 1.66 x mean RR, samples (0 = none yet) */
    uint16_t hr;
    uint8_t last_frac, sb_frac;
    uint8_t rr5_n, rr5_i, rr8_n, rr8_i;
    int8_t pol;                    /* polarity vote, >= 0 means positive R */
    uint8_t flags;                 /* internal state bits */
} dsp_qrs_t;

/* start_index: stream index of the first sample that will be passed. */
void dsp_qrs_init(dsp_qrs_t *q, uint32_t start_index);

/* Re-contact / gain change: forget thresholds, beats and filter state and
 * re-learn; the sample index keeps counting. */
void dsp_qrs_reset(dsp_qrs_t *q);

/* A gap (the front end on a rail): advance the timeline by n samples without
 * processing them. The filters re-prime on the next processed sample; the
 * learned thresholds, R polarity and RR history are kept, so detection
 * resumes at once after a short excursion. The first beat after the gap has
 * rr_ms = 0 (no interval across it). Call dsp_qrs_reset() for a new contact
 * or a gain change. */
void dsp_qrs_skip(dsp_qrs_t *q, uint32_t n);

/* Process n stream samples; writes up to max_beats detected beats (in R-peak
 * order) and returns how many. n <= 100 can produce at most 2 beats. */
size_t dsp_qrs_process(dsp_qrs_t *q, const int16_t *x, size_t n,
                       dsp_qrs_beat_t *beats, size_t max_beats);

/* Current median-of-5 heart rate, 0 until available. */
uint16_t dsp_qrs_hr(const dsp_qrs_t *q);

/* Spread of the last 5 RR intervals, (max - min) / median in percent;
 * 255 if fewer than 5 intervals. Regularity input to dsp_ecg_quality(). */
uint8_t dsp_qrs_rr_spread_pct(const dsp_qrs_t *q);

/* Samples since the last accepted beat (saturating), for quality / timeouts. */
uint32_t dsp_qrs_since_beat(const dsp_qrs_t *q);

#endif
