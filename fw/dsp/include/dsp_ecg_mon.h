/* ECG contact and signal monitoring: lead-off / re-contact on raw codes,
 * baseline wander, the 0..4 quality score and the auto-gain advisor. */
#ifndef DSP_ECG_MON_H
#define DSP_ECG_MON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Lead-off (raw 4 kHz codes) ------------------------------------------
 * "Rail" = code < 5 % or >= 95 % of full scale (< 205 or >= 3891).
 * LEAD OFF: rail samples for more than 100 ms (400 samples); gaps shorter
 *   than 20 ms (mains riding on the rail) do not restart the count.
 * CONTACT ("came off the rail"): 20 ms (80 samples) continuously off the rail
 *   while lead-off. Pulse FAST_RESTORE and reset the processing on this.
 * The state starts lead-off, so the first contact is reported as an event.
 * Rail samples while in contact are counted as clipping (gain advisor,
 * quality). The baseline is an EMA (tau 128 ms) of code - mid. */
#define DSP_LEAD_RAIL_LO   205u
#define DSP_LEAD_RAIL_HI   3891u
#define DSP_LEAD_OFF_RUN   400u
#define DSP_LEAD_GAP_RUN   80u

#define DSP_LEAD_EV_OFF     0x01u /* contact lost (lead-off asserted) */
#define DSP_LEAD_EV_CONTACT 0x02u /* came off the rail: contact made */

typedef struct {
    int32_t base_q9;   /* EMA of (code - mid), 9 fractional bits */
    uint32_t clip;     /* rail samples while in contact, since last take */
    uint16_t rail_run; /* rail samples in the current run (gaps < 20 ms ok) */
    uint16_t ok_run;   /* consecutive off-rail samples */
    uint16_t sec_n;    /* samples into the current second */
    uint16_t sec_rail; /* rail samples in the current second */
    uint16_t last_s;   /* rail samples in the last complete second */
    int16_t mid;
    uint8_t lead_off;
} dsp_lead_t;          /* 24 bytes */

void dsp_lead_init(dsp_lead_t *l, uint16_t mid_code);

/* Feed raw codes; returns the OR of DSP_LEAD_EV_* that occurred in the block. */
uint8_t dsp_lead_push(dsp_lead_t *l, const uint16_t *raw, size_t n);

bool dsp_lead_is_off(const dsp_lead_t *l);

/* Baseline offset from mid, ADC codes (signed). The start sequence pulses
 * FAST_RESTORE again if |offset| > 1024 (25 % of full scale) 500 ms after
 * contact: see dsp_lead_far_from_mid(). */
int16_t dsp_lead_baseline(const dsp_lead_t *l);
bool dsp_lead_far_from_mid(const dsp_lead_t *l);

/* Rail samples while in contact since the previous call (then cleared). */
uint32_t dsp_lead_take_clip(dsp_lead_t *l);

/* Rail samples (any state) in the last complete second of 4000 raw samples. */
uint16_t dsp_lead_rail_last_s(const dsp_lead_t *l);

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
 * 0 = no contact (lead-off) or saturated: more than 5 % of the raw samples
 *     of the last second on the rail.
 * Otherwise start at 4 and subtract:
 *   clipping      any rail sample in the last second             -1
 *   mains         residual > 100 uVpp: -1;  > 400 uVpp: -2
 *   wander        2 s baseline range > 500 uV: -1;  > 1500 uV: -2
 *   regularity    no beat for > 3 s, or RR spread > 30 %          -1
 *                 (ignored while settling: fewer than 5 intervals so far
 *                 and less than 3 s since contact)
 * and clamp to 1..4 while in contact. */
typedef struct {
    bool lead_off;
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
