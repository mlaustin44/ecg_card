/* Synthetic test signals for ecg_dsp (host tests only; uses double / malloc).
 *
 * Beat schedule  : true R-peak times with configurable rate, sinusoidal RSA and
 *                  random RR variation, plus optional ectopic beats.
 * ECG            : P-QRS-T Gaussian-sum morphology (QT scaled with sqrt(RR)),
 *                  mains, baseline wander, EMG-like band-limited noise, white
 *                  noise, optional analog front-end filters (0.32 / 0.48 Hz
 *                  HP, 159 / 339 Hz LP, exact first-order responses), ADC at 4 kHz
 *                  (sample n at n * 250 us) with dither and clipping.
 * PPG            : pulse wave whose onset follows each R-peak by a fixed PAT,
 *                  reflected photocurrent proportional to LED current, ambient
 *                  light with 100 Hz flicker, TIA / PPG_HI / ADC model with
 *                  the board's frame timing and DAC12 offset. */
#ifndef SYNTH_H
#define SYNTH_H

#include <stddef.h>
#include <stdint.h>

#include "dsp_ppg.h"
#include "dsp_ppg_ctrl.h"

/* ---- random ----------------------------------------------------------- */
typedef struct {
    uint64_t s;
    int have;
    double spare;
} synth_rng_t;
void synth_rng_seed(synth_rng_t *r, uint64_t seed);
double synth_uniform(synth_rng_t *r); /* [0, 1) */
double synth_gauss(synth_rng_t *r);   /* N(0, 1) */

/* ---- beat schedule ----------------------------------------------------- */
typedef struct {
    double hr_bpm;
    double rsa;        /* fractional RR modulation amplitude (sinusoid, 0.25 Hz) */
    double rr_sd;      /* fractional random RR s.d. */
    double ectopic_every; /* 0 = none; else every Nth beat premature by 30 % */
    double t_first;    /* first R time, s */
    double duration;   /* s */
    uint64_t seed;
} synth_beats_cfg_t;

typedef struct {
    double *t;   /* R times, s */
    size_t n;
} synth_beats_t;

void synth_beats_make(synth_beats_t *b, const synth_beats_cfg_t *c);
void synth_beats_free(synth_beats_t *b);

/* ---- ECG ---------------------------------------------------------------- */
typedef struct {
    double amp_mv;     /* R amplitude */
    double t_ratio;    /* T amplitude / R amplitude (0.3 normal) */
    double mains_uv;   /* mains amplitude, uV peak (input-referred) */
    double mains_hz;
    double wander_uv;  /* baseline wander, uV peak (0.3 Hz + 0.07 Hz) */
    double emg_uv;     /* EMG-like noise, uV rms (20-150 Hz) */
    double white_uv;   /* white noise, uV rms */
    int gain;          /* 558 or 1098 */
    double mid;        /* mid code */
    int analog;        /* simulate the analog front end */
    uint64_t seed;
} synth_ecg_cfg_t;

typedef struct {
    synth_ecg_cfg_t c;
    const synth_beats_t *beats;
    size_t bi;         /* first beat that can still contribute */
    uint64_t n;        /* next raw sample index */
    synth_rng_t rng;
    double lp1, lp2, hp1_x, hp2_x; /* analog filter state */
    double x_prev, h1_prev, h2_prev, l1_prev;
    double emg_hp_x, emg_hp_y, emg_lp;
    double mains_phase;
    int analog_primed;
} synth_ecg_t;

void synth_ecg_init(synth_ecg_t *e, const synth_ecg_cfg_t *c, const synth_beats_t *b);
/* Input-referred clean ECG (mV) at time t, s (no noise, no filters). */
double synth_ecg_clean_mv(const synth_ecg_t *e, double t);
/* Next ADC0 code (4 kHz). */
uint16_t synth_ecg_next(synth_ecg_t *e);

/* ---- PPG ---------------------------------------------------------------- */
typedef struct {
    double pat_s;        /* R-peak -> pulse foot (intersecting tangent) */
    double refl_ua_per_ma; /* DC photocurrent per mA of LED current */
    double pi;           /* pulsatile fraction (AC / DC) */
    double ambient_ua;   /* ambient photocurrent */
    double flicker;      /* 100 Hz flicker, fraction of ambient */
    double flicker_hz;
    double noise_na;     /* photocurrent noise rms per sample */
    double resp;         /* respiratory DC modulation fraction (0.2 Hz) */
    uint64_t seed;
} synth_ppg_cfg_t;

typedef struct {
    synth_ppg_cfg_t c;
    const synth_beats_t *beats;
    double foot_off;     /* onset -> isolated-pulse tangent foot, s */
    uint32_t k;          /* next frame */
    synth_rng_t rng;
} synth_ppg_t;

void synth_ppg_init(synth_ppg_t *p, const synth_ppg_cfg_t *c, const synth_beats_t *b);
/* Normalised blood-volume (0 at diastole, ~1 at systolic peak) at time t. */
double synth_ppg_volume(const synth_ppg_t *p, double t);
/* Photocurrent (uA) at time t for LED current led_ma, without ambient/noise. */
double synth_ppg_signal_ua(const synth_ppg_t *p, double t, double led_ma);
/* Next frame (k-th frame: dark at k*10 ms + 9.70 ms, on at + 9.95 ms). */
void synth_ppg_next(synth_ppg_t *p, uint16_t led_code, uint16_t dac12, dsp_ppg_frame_t *f);
/* True intersecting-tangent foot of beat i on the noiseless light signal
 * (sampled at 10 kHz), s; < 0 if not found. */
double synth_ppg_true_foot(const synth_ppg_t *p, size_t i);

/* ---- closed-loop PPG acquisition (what fw/acq will do) ------------------
 * Every frame: synthesise with the current LED / DAC12 codes, process, flag
 * the frame after any change. Every dcc_every frames: DC cancellation from
 * dsp_ppg_take_avg(); every agc_every frames: one AGC decision (skipped if a
 * step happened in the last 5 frames). Extra forced steps can be injected by
 * the caller by changing led / dac and setting pending flags. */
typedef struct {
    dsp_ppg_t ppg;
    dsp_agc_t agc;
    uint16_t led, dac, max_code;
    uint8_t pending;     /* DSP_PPG_IN_* for the next frame */
    uint32_t k;
    uint32_t last_step;
    int agc_every, dcc_every;
    int agc_steps, dcc_steps;
    uint32_t agc_tia_sum, agc_dark_sum, agc_n;
    int predict_dcc;     /* re-cancel the offset with each AGC step */
    dsp_ppg_avg_t avg;
} synth_ppg_loop_t;

void synth_ppg_loop_init(synth_ppg_loop_t *l, uint16_t led, uint8_t limit_ma);
bool synth_ppg_loop_frame(synth_ppg_loop_t *l, synth_ppg_t *src, dsp_ppg_frame_t *f,
                          dsp_ppg_out_t *out, dsp_ppg_pulse_t *pulse);

/* ---- measurement helpers ------------------------------------------------ */
/* Amplitude of the component at f (Hz) in x[0..n) sampled at fs (LSQ fit). */
double synth_tone_amp(const double *x, size_t n, double fs, double f, double *phase);

#endif
