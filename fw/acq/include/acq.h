/* ECG / PPG acquisition: start sequencer, per-sample processing (fw/dsp call
 * order of fw/dsp/README.md), contact detection with FAST_RESTORE, settling,
 * PPG control loops (DC cancellation, LED AGC) and session statistics.
 *
 * Non-blocking: acq_start() begins the start sequence, acq_poll() (every
 * main-loop pass, at least every ~50 ms while running) advances it and
 * drains the HAL rings, acq_stop() runs the stop sequence (rails_off()) at
 * once. Outputs go to the sink callbacks (stream frames, display samples,
 * events); this module knows nothing about the UI or the stream format.
 *
 * ECG contact model (ECG and BOTH), from the front end's time constants
 * (ecg-fw.md, ECG chain): U5 gain 21 with a tau1 = 0.5 s AC coupling that
 * FAST_RESTORE does not reach; FAST_RESTORE only resets C34/R36. Every touch
 * or thumb movement steps the electrode offset (tens to hundreds of mV with
 * dry thumbs); x21 it saturates U5, and while it decays its slope alone
 * rails the output again tens of ms after any FAST_RESTORE pulse, for ~2 s
 * (4 tau1 for 150 mV). Floating inputs (thumbs off) also sit on a rail with
 * brief in-range excursions. So a rail means "recovering", never by itself
 * "thumbs off"; only a STABLE signal (dsp_ecg_mon.h: in range continuously
 * for DSP_LEAD_STABLE_MS, FAST_RESTORE pulses and DSP_LEAD_BLANK_MS after
 * them excluded) is evidence of contact.
 *   WAIT    no contact. -> SETTLE on a stable signal.
 *   SETTLE  "settling": in-range time accumulates; a rail excursion shorter
 *           than ACQ_ECG_BLIP_MS pauses it, a longer one starts it over.
 *           RUNNING once ECG_SETTLE_MS have accumulated (the first-stage
 *           tail, brief step 8) and a beat was accepted (ACQ_ECG_MIN_QRS_UV)
 *           at most ACQ_ECG_NO_BEAT_MS of in-range time ago. -> WAIT after
 *           ACQ_ECG_LOST_WAIT_MS without a stable signal, or when settling
 *           completed but ACQ_ECG_NO_BEAT_MS of in-range signal passed without
 *           a beat (open inputs that sit quietly at mid-scale must not pass as
 *           a measurement; time on a rail does not count).
 *   RUN     settled: display samples, session statistics. A rail excursion
 *           shorter than ACQ_ECG_BLIP_MS is a blip: RUN continues, the trace
 *           shows a gap (sink ecg_gap) and the QRS detector resumes with its
 *           learned thresholds (dsp_qrs_skip). -> HOLD when the output stays
 *           on a rail for ACQ_ECG_BLIP_MS, or there is no stable signal for
 *           ACQ_ECG_BLIP_MS + DSP_LEAD_STABLE_MS (choppy: floating inputs).
 *   HOLD    settling again after RUNNING began ("Hold still"): the session
 *           continues. -> RUN after ACQ_ECG_RESUME_MS in range with a beat
 *           in that time.
 *   END     end of contact after RUNNING began: no stable signal for
 *           ACQ_ECG_LOST_END_MS (floating inputs make short in-range
 *           excursions, so this is not a rail-run length), or no accepted beat
 *           in ACQ_ECG_NO_BEAT_MS of in-range signal (RUN) /
 *           ACQ_ECG_HOLD_END_MS of wall time (HOLD). The app
 *           shows the summary.
 * Brief excursions while waiting, or floating inputs, therefore never reach
 * SETTLE, and a thumb movement while RUNNING costs a gap in the trace, or
 * about a second of HOLD after a longer one. The QRS detector learns from
 * each new contact (WAIT -> SETTLE); after an excursion it resumes (beats
 * are reported from SETTLE on).
 * FAST_RESTORE: pulsed (ECG_FAST_RESTORE_MS) while the output has been on a
 *   rail for ACQ_FR_RAIL_MS, at most every ACQ_FR_REPEAT_MS, so it is back in
 *   range as soon as U5 has decayed far enough; in range only once,
 *   ACQ_FR_RECHECK_MS after the signal became stable, if the baseline is
 *   still > 25 % of full scale from mid (brief start step 7). Never before
 *   the electrodes are connected (start step 4): acquisition starts after
 *   MEASURE_REQ.
 *   That is acq_cfg_t.restore = RESTORE_FAST. RESTORE_SLOW pulses only after
 *   ACQ_FR_SLOW_RAIL_MS continuously railed, at most every
 *   ACQ_FR_SLOW_REPEAT_MS, and skips the in-range recheck; RESTORE_OFF never
 *   pulses (settings.h; bench A/B: on the first board each release may leave
 *   a random-sign step at the high-pass node).
 * PPG: contact = the finger detector; settled ACQ_PPG_SETTLE_MS later.
 * BOTH: the session follows ECG contact; PPG values are reported while
 *   the finger is detected.
 * "Settled" is the UI's RUNNING state: display samples are only delivered,
 * and session statistics only accumulated, while settled. */
#ifndef ACQ_H
#define ACQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "dsp_ecg_mon.h"
#include "dsp_hrv.h"

#define ACQ_PPG_SETTLE_MS  2000u
/* VMID wait before the self-check: the brief's 300 ms minimum is 6 time
 * constants (0.25 % = 4 mV of the 10 mV budget still missing); 400 ms
 * leaves 0.03 %. */
#define ACQ_VMID_WAIT_MS   (ECG_VMID_SETTLE_MS + 100u)
#define ACQ_FR_RECHECK_MS  500u   /* FAST_RESTORE check after the signal became stable */
/* FAST_RESTORE while railed: after 50 ms on the rail (not for mains or QRS
 * clipping), then every 250 ms = tau1 / 2, so the output is back in range at
 * most 250 ms (a 40 % decay of U5's tail) after it could be. */
#define ACQ_FR_RAIL_MS     50u
#define ACQ_FR_REPEAT_MS   250u
#define ACQ_FR_SLOW_RAIL_MS   2000u /* RESTORE_SLOW: 4 tau1, U5's tail mostly gone */
#define ACQ_FR_SLOW_REPEAT_MS 2000u
/* Contact timing, multiples of U5's tau1 = 0.5 s: a 150 mV offset step
 * needs ~1.5 s (3 tau1) before a pulse leaves the output in range, + 0.5 s
 * to be stable. */
/* SETTLE (never RUNNING): no stable signal -> WAIT. Longer than the rail after
 * a 150 mV thumb adjustment (about 3 s), so adjusting does not mean "place thumbs". */
#define ACQ_ECG_LOST_WAIT_MS 4000u
#define ACQ_ECG_LOST_END_MS  4000u /* after RUNNING began: no stable signal -> END */
/* A rail excursion shorter than this is a blip: RUN continues with a gap in
 * the trace (a thumb shifting; at 558x the window is only +-2.9 mV). */
#define ACQ_ECG_BLIP_MS      2000u
#define ACQ_ECG_RESUME_MS    1000u /* HOLD -> RUN: in range this long, with a beat */
#define ACQ_ECG_HOLD_END_MS  8000u /* HOLD: no accepted beat this long -> END */
/* No accepted beat for 5 s (2.5 beats at 30 bpm) while stable: not a person.
 * "Accepted" = a QRS of at least ACQ_ECG_MIN_QRS_UV input-referred (a thumb
 * lead I QRS is 0.5-1.5 mV); the detector's adaptive thresholds also find
 * "beats" in the quantisation steps of open inputs sitting at mid-scale. */
#define ACQ_ECG_NO_BEAT_MS   5000u
#define ACQ_ECG_MIN_QRS_UV   100u
#define ACQ_VLED_WAIT_MS   10u    /* VLED_EN -> LED current (>= PPG_VLED_START_MS) */
#define ACQ_AGC_FRAMES     25u    /* one AGC decision per 250 ms */
#define ACQ_DCC_FRAMES     100u   /* one DC-cancel update per second */
/* PPG: pulse_bpm, pi_permille, pat_ms (and hr_bpm in PPG-only mode) read 0
 * ("not available") once no pulse has been detected for this long, instead
 * of freezing at the last value. 5 s = 2.5 beats at 30 bpm. */
#define ACQ_PULSE_TIMEOUT_MS 5000u

/* ECG auto-gain (acq_cfg_t.gain_auto): the session starts at ECG_GAIN_LO.
 * Up once to ECG_GAIN_HI after ACQ_GAIN_WIN_MS of RUN with at least
 * ACQ_GAIN_MIN_BEATS beats, no rail sample, and the largest excursion from
 * mid (QRS + wander) still within ACQ_GAIN_UP_PCT % of half-scale once
 * multiplied by ECG_GAIN_HI / ECG_GAIN_LO. Down at once when the output at
 * ECG_GAIN_HI leaves the range for longer than a clip (a blip, or HOLD), or
 * the excursion passes ACQ_GAIN_DOWN_PCT % of half-scale; after a step down
 * the gain stays low for the session. A switch re-learns the QRS
 * detector (about 2 s) but keeps the session running. */
#define ACQ_GAIN_WIN_MS    5000u
#define ACQ_GAIN_MIN_BEATS 4u
#define ACQ_GAIN_UP_PCT    60u
#define ACQ_GAIN_DOWN_PCT  90u
#define ACQ_GAIN_BLANK_MS  20u   /* samples after a switch: no evidence */

typedef struct {
    bool ecg, ppg;
    bool gain_hi;         /* ECG gain ECG_GAIN_HI (fixed, or the start if gain_auto) */
    bool gain_auto;       /* ECG auto-gain (above); starts at ECG_GAIN_LO */
    uint8_t rld;          /* rld_mode_t */
    uint8_t notch;        /* notch_mode_t (AUTO: 60 Hz until the detector decides) */
    uint8_t led_limit_ma; /* per-LED limit for the AGC */
    uint8_t restore;      /* restore_mode_t (ECG FAST_RESTORE policy) */
} acq_cfg_t;

typedef enum {
    ACQ_IDLE,     /* rails off */
    ACQ_STARTING, /* start sequence in progress (VMID settling etc.) */
    ACQ_ON,       /* streams running */
    ACQ_FAILED    /* start failed: rails off again, see err */
} acq_phase_t;

typedef enum {
    ACQ_ERR_NONE,
    ACQ_ERR_VMID,    /* VMID_MON outside +-ECG_VMID_TOL_MV of 1650 mV */
    ACQ_ERR_LED_DAC, /* MCP4728 did not accept the LED current */
    ACQ_ERR_USB,     /* USB power appeared during the ECG start sequence */
    ACQ_ERR_START    /* a rail precondition failed (should not happen) */
} acq_err_t;

typedef enum { ACQ_ECG_WAIT, ACQ_ECG_SETTLE, ACQ_ECG_RUN, ACQ_ECG_HOLD, ACQ_ECG_END } acq_ecg_state_t;
/* Why the last ECG state change happened (for the log). */
typedef enum {
    ACQ_WHY_NONE,
    ACQ_WHY_STABLE,    /* WAIT -> SETTLE */
    ACQ_WHY_SETTLED,   /* -> RUN: settled with a beat */
    ACQ_WHY_RAIL,      /* RUN -> HOLD: stability lost */
    ACQ_WHY_NO_STABLE, /* -> WAIT / END: no stable signal for the limit */
    ACQ_WHY_NO_BEAT    /* -> WAIT / END: no beat for ACQ_ECG_NO_BEAT_MS */
} acq_why_t;

typedef struct {
    acq_phase_t phase;
    acq_err_t err;
    uint16_t vmid_mv;        /* last VMID self-check */
    bool contact;            /* primary contact (see above) */
    bool settled;            /* RUNNING */
    uint8_t settle_pct;      /* 0..100 while contact && !settled */
    uint32_t contact_ms;     /* hal_millis() of the last primary contact edge */
    bool finger;             /* PPG finger detected */
    uint16_t hr_bpm;         /* ECG median-of-5, or the pulse rate in PPG mode */
    uint16_t pulse_bpm, pat_ms, pi_permille; /* 0 = n/a (also ACQ_PULSE_TIMEOUT_MS after the last pulse) */
    uint8_t quality;         /* 0..4 */
    uint8_t beat_seq;        /* +1 per ECG beat (ECG, BOTH) or pulse (PPG) */
    bool gain_hi;
    uint8_t gain_switches;   /* auto-gain changes this session */
    uint8_t notch_hz;        /* notch in use */
    uint16_t mid_code;       /* VMID_MON code (ECG modes) */
    uint16_t led_code, dac12;
    uint32_t running_ms;     /* total settled time this session */
    uint32_t ecg_samples;    /* 500 Hz stream index of the next sample */
    uint32_t ppg_frames;
    uint8_t ecg_state;       /* acq_ecg_state_t (ECG, BOTH) */
    uint8_t ecg_why;         /* acq_why_t of the last change */
    bool railed;             /* dsp_lead_railed() now */
    bool blip;               /* RUN, output on a rail for less than ACQ_ECG_BLIP_MS so far */
    uint16_t blips;          /* blips this session */
    uint16_t qrs_uv;         /* median QRS peak-to-peak of the last 5 accepted beats, uV (0 = none) */
    bool qrs_neg;            /* R waves point down (detector polarity vote) */
    uint16_t mains_uvpp;     /* dsp_mains_residual_uvpp(), last 0.5 s window */
    uint16_t ecg_beats;      /* beats reported this session */
    bool contact_beat;       /* an accepted beat (ACQ_ECG_MIN_QRS_UV) in the current / last contact */
    uint16_t ppg_pulses;     /* pulses detected this session */
    uint16_t agc_steps;      /* LED current changes by the AGC */
    uint16_t dcc_steps;      /* DAC12 offset changes */
} acq_status_t;

/* Diagnostics since the previous acq_diag_take() (the app logs one line per
 * second). ECG: every raw 4 kHz code (incl. re-filled ones). PPG: measured
 * frames only. */
typedef struct {
    dsp_lead_stats_t ecg;
    uint16_t fr_pulses;
    uint16_t ppg_n;
    uint32_t dark_sum, on_sum, hi_sum; /* tia_dark, tia_on, hi_on */
    int32_t coarse_sum;                /* tia_on - tia_dark */
    uint16_t hi_min, hi_max;
} acq_diag_t;

typedef struct {
    dsp_hrv_summary_t hrv;   /* ECG beats while settled */
    uint16_t pulses;         /* PPG pulses while settled */
    uint16_t pulse_avg;      /* bpm from the mean pulse interval, 0 = n/a */
    uint16_t pi_avg;         /* permille, 0 = n/a */
    uint16_t pat_avg_ms;     /* 0 = n/a */
    uint16_t pat_n;
    uint32_t running_ms;
    int8_t gain_advice;      /* +1 up, -1 down, 0 keep (dsp_gain advisor) */
    uint32_t ecg_lost, ppg_lost; /* samples / frames lost (timeline gaps, re-filled) */
    uint16_t ecg_resyncs, ppg_resyncs; /* gaps too long to re-fill (> 2.5 s): index slipped */
    uint32_t i2c_errors;     /* LED DAC writes that failed */
    bool gain_hi;
    uint8_t notch_hz;
    uint16_t mid_code, led_code, dac12;
    /* ECG front end over the whole session (dsp_lead_totals_t: samples not
     * blanked by FAST_RESTORE) */
    uint32_t ecg_raw_n, ecg_rail_n;
    uint32_t ecg_run_max_ms;  /* longest in-range run */
    uint32_t fr_pulses;       /* FAST_RESTORE pulses */
} acq_summary_t;

typedef struct {
    /* STREAM_ECG_BLOCK: 10 samples starting at stream index start.
     * flags: bit0 gain_hi, bit1 settling, bit2 lead_off */
    void (*ecg_block)(uint32_t start, uint8_t flags, const int16_t *s);
    /* STREAM_PPG_BLOCK: cf = {coarse0, fine0, coarse1, fine1}.
     * flags: bit0/1 frame disturbed, bit2 no finger */
    void (*ppg_block)(uint32_t start, uint16_t led_code, uint8_t flags, const int16_t *cf);
    /* Display samples while settled: ECG uV at 500 Hz, PPG band-passed at 100 Hz. */
    void (*ecg_trace)(const int16_t *uv, size_t n);
    void (*ppg_trace)(const int16_t *v, size_t n);
    void (*beat)(uint32_t index, uint16_t hr_bpm);
    void (*pulse)(uint32_t foot_ecg_index, uint16_t pulse_bpm, uint16_t pat_ms, uint16_t pi_permille);
    void (*contact)(bool on);  /* primary contact edges */
    void (*settled)(bool on);  /* RUNNING begins / ends */
    /* ECG samples while settled that have no display value (a blip: the
     * output on a rail), in place of ecg_trace, so the sweep keeps time. */
    void (*ecg_gap)(size_t n);
} acq_sink_t;

/* sink may be NULL or have NULL members. */
void acq_init(const acq_sink_t *sink);

/* Begin the start sequence (rails up). Returns false if already active or
 * the configuration is empty. */
bool acq_start(const acq_cfg_t *cfg);
void acq_poll(uint32_t now_ms);
/* Stop sequence now (rails_off()); freezes the summary. Idempotent. */
void acq_stop(void);
/* True from acq_start() until acq_stop() (STARTING or ON). */
bool acq_active(void);

const acq_status_t *acq_status(void);
/* Copy and restart the diagnostics accumulators. */
void acq_diag_take(acq_diag_t *d);
/* Summary of the last (or current) session. */
const acq_summary_t *acq_summary(void);

#endif
