/* Synthetic ECG + PPG world for simulator scenario tests (host only).
 * Wraps fw/dsp/tests/synth.c: one beat schedule drives the ECG source
 * (electrode microvolts) and the PPG source (photocurrent per mA of LED
 * current, pulse foot = R-peak + PAT), both on the simulator's absolute
 * time axis. */
#ifndef TESTSRC_H
#define TESTSRC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "synth.h"

typedef struct {
    double hr_bpm;      /* mean rate */
    double rr_sd;       /* fractional random RR s.d. (HRV) */
    double rsa;         /* respiratory modulation (fraction) */
    double t_first;     /* first R-peak, s of sim time */
    double duration;    /* s */
    double amp_mv;      /* R amplitude */
    double pat_s;       /* R -> pulse foot */
    double refl_ua_per_ma; /* PPG DC photocurrent per mA LED */
    double pi;          /* PPG AC/DC */
    uint64_t seed;
} testsrc_cfg_t;

/* Defaults: 72 bpm, rr_sd 0.03, 1.0 mV, PAT 200 ms, 0.15 uA/mA, PI 2 %. */
testsrc_cfg_t testsrc_defaults(void);
/* Build the beat schedule and install both sources in the simulator. */
void testsrc_install(const testsrc_cfg_t *c);
/* false: the PPG source holds its DC level (finger on, no visible pulse). */
void testsrc_ppg_pulsatile(bool on);
const synth_beats_t *testsrc_beats(void);
const synth_ppg_t *testsrc_ppg(void);

/* Truth over R-peaks in [t0, t1] s: NN statistics as dsp_hrv defines them
 * (all intervals; the synthetic schedule has no ectopics). */
typedef struct {
    double hr_avg, hr_min5, hr_max5; /* bpm; min/max of the median-of-5 rate */
    double sdnn_ms, rmssd_ms;
    int beats;
} testsrc_truth_t;
testsrc_truth_t testsrc_truth(double t0, double t1);
/* Mean true PAT (R-peak to intersecting-tangent foot) over beats in [t0, t1]. */
double testsrc_true_pat_ms(double t0, double t1);

#endif
