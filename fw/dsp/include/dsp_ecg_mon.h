/* ECG front-end monitoring: rail / in-range / stable on raw codes, baseline,
 * code statistics, baseline wander, the 0..4 quality score and the auto-gain
 * advisor. */
#ifndef DSP_ECG_MON_H
#define DSP_ECG_MON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Front-end state (raw 4 kHz codes) -----------------------------------
 * "Rail" = code < 5 % or >= 95 % of full scale (< 205 or >= 3891).
 * RAILED: rail samples for more than 20 ms (80 samples); gaps shorter than
 *   20 ms (mains riding on the rail) do not end the run, 20 ms continuously
 *   in range does (DSP_LEAD_EV_INRANGE). A rail means the front end is
 *   saturated or recovering (an electrode offset step decays through U5's
 *   0.5 s coupling for ~2 s), or the inputs float: it never means "thumbs
 *   off" by itself. Shorter rail runs are clipping (gain advisor, quality).
 * STABLE: continuously in range for DSP_LEAD_STABLE_MS (DSP_LEAD_EV_STABLE),
 *   short clips included; lost when RAILED asserts (DSP_LEAD_EV_UNSTABLE). One
 *   tau of U5, and longer than the in-range excursions floating inputs make
 *   (up to ~0.3 s on the first board).
 * BLANKED samples (FAST_RESTORE held, and DSP_LEAD_BLANK_MS after it; the
 *   caller passes how many of a block are blanked) are not evidence either
 *   way: the pulse forces mid-scale. They pause every count and the
 *   baseline; the min / max / rail statistics still see them.
 * The state starts railed with an empty run: no evidence yet.
 * The baseline is an EMA (tau 128 ms) of code - mid. */
#define DSP_LEAD_RAIL_LO   205u
#define DSP_LEAD_RAIL_HI   3891u
#define DSP_LEAD_RAIL_RUN  80u    /* rail samples (20 ms) -> RAILED */
#define DSP_LEAD_OK_RUN    80u    /* in-range samples (20 ms) that end a rail run */
#define DSP_LEAD_STABLE_MS 500u
#define DSP_LEAD_STABLE_RUN (DSP_LEAD_STABLE_MS * 4u)
#define DSP_LEAD_BLANK_MS  10u    /* after FAST_RESTORE is released */

#define DSP_LEAD_EV_RAIL     0x01u /* RAILED asserted */
#define DSP_LEAD_EV_INRANGE  0x02u /* rail run ended: in range for 20 ms */
#define DSP_LEAD_EV_STABLE   0x04u /* in range for DSP_LEAD_STABLE_MS */
#define DSP_LEAD_EV_UNSTABLE 0x08u /* RAILED asserted while stable */

typedef struct {
    uint32_t n, rail, sum;
    uint32_t run_max;  /* longest in-range run of unblanked samples (a run
                        * still going at the take counts so far) */
    uint16_t min, max;
} dsp_lead_stats_t;   /* raw codes since the last dsp_lead_take_stats() */

/* Whole-run totals since dsp_lead_init(), unblanked samples only (bench A/B
 * of the FAST_RESTORE policies: blanked samples are forced to mid-scale). */
typedef struct {
    uint32_t n, rail;
    uint32_t run_max;  /* longest in-range run, samples */
} dsp_lead_totals_t;

typedef struct {
    int32_t base_q9;   /* EMA of (code - mid), 9 fractional bits */
    uint32_t clip;     /* rail samples while stable, since last take */
    uint32_t rail_run; /* rail samples in the current run (gaps < 20 ms ok) */
    uint32_t in_run;   /* samples since the rail run ended (0 while railed) */
    uint16_t ok_run;   /* consecutive in-range samples */
    uint16_t sec_n;    /* samples into the current second */
    uint16_t sec_rail; /* rail samples in the current second */
    uint16_t last_s;   /* rail samples in the last complete second */
    uint32_t run_cur;  /* current in-range run of unblanked samples */
    int16_t mid;
    uint8_t railed, stable;
    dsp_lead_stats_t st;
    dsp_lead_totals_t tot;
} dsp_lead_t;

void dsp_lead_init(dsp_lead_t *l, uint16_t mid_code);

/* Feed raw codes; the first n_blank of them are blanked (see above).
 * Returns the OR of DSP_LEAD_EV_* that occurred in the block. */
uint8_t dsp_lead_push(dsp_lead_t *l, const uint16_t *raw, size_t n, size_t n_blank);

bool dsp_lead_railed(const dsp_lead_t *l);
bool dsp_lead_stable(const dsp_lead_t *l);
/* Length of the current rail run while railed (else 0), and of the current
 * in-range run while not railed (else 0), in raw samples (4 per ms). */
uint32_t dsp_lead_rail_samples(const dsp_lead_t *l);
uint32_t dsp_lead_in_samples(const dsp_lead_t *l);

/* Baseline offset from mid, ADC codes (signed). The start sequence pulses
 * FAST_RESTORE again if |offset| > 1024 (25 % of full scale) 500 ms after
 * the signal became stable: see dsp_lead_far_from_mid(). */
int16_t dsp_lead_baseline(const dsp_lead_t *l);
bool dsp_lead_far_from_mid(const dsp_lead_t *l);

/* Rail samples while stable since the previous call (then cleared). */
uint32_t dsp_lead_take_clip(dsp_lead_t *l);

/* Rail samples (any state) in the last complete second of 4000 raw samples. */
uint16_t dsp_lead_rail_last_s(const dsp_lead_t *l);

/* Statistics of every code pushed since the previous call (then cleared):
 * count, rail samples, sum, min, max (min > max when n = 0). Counting
 * stops after 2^20 samples (262 s) if nobody takes them. */
void dsp_lead_take_stats(dsp_lead_t *l, dsp_lead_stats_t *out);
const dsp_lead_totals_t *dsp_lead_totals(const dsp_lead_t *l);

/* ---- Baseline wander (500 Hz stream) --------------------------------------
 * Mean of each 0.5 s block; wander = max - min of the last 4 block means
 * (2 s), input-referred uV. */
typedef struct {
    int32_t acc;
    int16_t mean[4];
    int32_t uv_q20;
    uint16_t n;
    uint8_t idx, cnt;
} dsp_wander_t;

void dsp_wander_init(dsp_wander_t *w, int gain);
void dsp_wander_push(dsp_wander_t *w, const int16_t *x, size_t n);
uint16_t dsp_wander_uvpp(const dsp_wander_t *w); /* 0 until 2 s seen */

/* ---- Quality score ---------------------------------------------------------
 * 0 = no contact / railed (lead_off) or saturated: more than 5 % of the raw
 *     samples of the last second on the rail.
 * Otherwise start at 4 and subtract:
 *   clipping      any rail sample in the last second             -1
 *   mains         residual > 100 uVpp: -1;  > 400 uVpp: -2
 *   wander        2 s baseline range > 500 uV: -1;  > 1500 uV: -2
 *   regularity    no beat for > 3 s, or RR spread > 30 %          -1
 *                 (ignored while settling: fewer than 5 intervals so far
 *                 and less than 3 s since contact)
 * and clamp to 1..4 while in contact. */
typedef struct {
    bool lead_off;           /* no contact, or dsp_lead_railed() */
    uint16_t rail_per_s;     /* dsp_lead_rail_last_s() (of 4000) */
    uint16_t mains_uvpp;     /* dsp_mains_residual_uvpp() */
    uint16_t wander_uvpp;    /* dsp_wander_uvpp() */
    uint8_t rr_spread_pct;   /* dsp_qrs_rr_spread_pct(), 255 = n/a */
    uint32_t ms_since_beat;  /* since the last beat, or since contact */
    uint32_t ms_since_contact;
} dsp_quality_in_t;

uint8_t dsp_ecg_quality(const dsp_quality_in_t *in);

/* ---- Auto-gain advisor -----------------------------------------------------
 * Feed each beat's QRS amplitude (dsp_qrs_beat_t.amp, stream units; full ADC
 * range = 32768) and, periodically, elapsed time and clip counts.
 *   at 558x: recommend UP if, over a full 5 s window, at least 3 beats were
 *            seen, every QRS was < 25 % of range (8192 units = 1.48 mV) and
 *            nothing clipped;
 *   at 1098x: recommend DOWN if any 5 s window had >= 3 clip events (clipping
 *            samples in >= 3 separate dsp_gain_tick() calls) or a QRS > 90 %
 *            of range.
 * The advice latches until dsp_gain_init() is called with the new gain (the
 * app switches gain between records only and pulses FAST_RESTORE after). */
typedef enum { DSP_GAIN_KEEP, DSP_GAIN_UP, DSP_GAIN_DOWN } dsp_gain_advice_t;

#define DSP_GAIN_WINDOW_MS 5000u
#define DSP_GAIN_UP_AMP    8192u
#define DSP_GAIN_DOWN_AMP  29491u

typedef struct {
    uint16_t win_ms;
    uint16_t max_amp;
    uint8_t beats;
    uint8_t clip_events;
    uint8_t gain_hi;
    uint8_t advice;
    uint8_t clipped;  /* any clipping in the current window */
} dsp_gain_t;

void dsp_gain_init(dsp_gain_t *g, bool gain_hi);
void dsp_gain_beat(dsp_gain_t *g, uint16_t qrs_amp);
/* Call every 10..1000 ms with the elapsed time and dsp_lead_take_clip(). */
dsp_gain_advice_t dsp_gain_tick(dsp_gain_t *g, uint16_t elapsed_ms, uint32_t clip_samples);

#endif
