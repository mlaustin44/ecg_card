/* Representative ui_model_t scenarios with synthetic ECG/PPG, shared by the
 * UI tests and host/render. Host only (uses double and libm). */
#ifndef UI_SCENARIOS_H
#define UI_SCENARIOS_H

#include <stdint.h>

#include "ui_model.h"

typedef struct {
    const char *name;
    void (*setup)(ui_model_t *m); /* calls ui_init(), fills m, feeds traces */
} scenario_t;

extern const scenario_t scenarios[];
extern const int n_scenarios;
const scenario_t *scenario_find(const char *name);

/* Baseline model: Home, 76 % on battery, default settings. */
void scen_defaults(ui_model_t *m);

/* Live signal simulation. */
typedef struct {
    unsigned hr_bpm;   /* heart rate */
    int ecg_uv;        /* R-wave amplitude, microvolts */
    int ppg_amp;       /* PPG pulse amplitude, arbitrary units */
    int pat_ms;        /* R-peak to PPG foot */
    int mains_uv;      /* residual 50 Hz after the notch */
    int wander_uv;     /* baseline wander */
    int noise_uv;      /* white noise, peak */
} sim_sig_t;

/* Start a measurement screen: ui_trace_reset(sweep) and signal at t = now_ms. */
void sim_begin(ui_model_t *m, const sim_sig_t *sig);
void sim_set(const sim_sig_t *sig); /* change the signal mid-run */
/* Advance in 40 ms frames: push ECG/PPG samples (per screen), update
 * beat_seq, now_ms, elapsed_s. */
void sim_run(ui_model_t *m, uint32_t ms);

#endif
