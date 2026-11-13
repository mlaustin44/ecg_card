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
 * Contact model
 *   ECG and BOTH: contact = the lead-off detector reports contact. On every
 *     contact edge FAST_RESTORE is pulsed (ECG_FAST_RESTORE_MS), and once
 *     more 500 ms later if the baseline is still > 25 % of full scale from
 *     mid (ecg-fw.md start sequence step 7). Settled ECG_SETTLE_MS after the
 *     contact edge.
 *   PPG: contact = the finger detector; settled ACQ_PPG_SETTLE_MS later.
 *   BOTH: the session follows ECG contact; PPG values are reported while
 *     the finger is detected.
 * "Settled" is the UI's RUNNING state: display samples are only delivered,
 * and session statistics only accumulated, while settled. */
#ifndef ACQ_H
#define ACQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h"
#include "dsp_hrv.h"

#define ACQ_PPG_SETTLE_MS  2000u
/* VMID wait before the self-check: the brief's 300 ms minimum is 6 time
 * constants (0.25 % = 4 mV of the 10 mV budget still missing); 400 ms
 * leaves 0.03 %. */
#define ACQ_VMID_WAIT_MS   (ECG_VMID_SETTLE_MS + 100u)
#define ACQ_FR_RECHECK_MS  500u   /* second FAST_RESTORE check after contact */
#define ACQ_VLED_WAIT_MS   10u    /* VLED_EN -> LED current (>= PPG_VLED_START_MS) */
#define ACQ_AGC_FRAMES     25u    /* one AGC decision per 250 ms */
#define ACQ_DCC_FRAMES     100u   /* one DC-cancel update per second */
/* PPG: pulse_bpm, pi_permille, pat_ms (and hr_bpm in PPG-only mode) read 0
 * ("not available") once no pulse has been detected for this long, instead
 * of freezing at the last value. 5 s = 2.5 beats at 30 bpm. */
#define ACQ_PULSE_TIMEOUT_MS 5000u

typedef struct {
    bool ecg, ppg;
    bool gain_hi;         /* ECG gain 1098x */
    uint8_t rld;          /* rld_mode_t */
    uint8_t notch;        /* notch_mode_t (AUTO: 60 Hz until the detector decides) */
    uint8_t led_limit_ma; /* per-LED limit for the AGC */
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
    uint8_t notch_hz;        /* notch in use */
    uint16_t mid_code;       /* VMID_MON code (ECG modes) */
    uint16_t led_code, dac12;
    uint32_t running_ms;     /* total settled time this session */
    uint32_t ecg_samples;    /* 500 Hz stream index of the next sample */
    uint32_t ppg_frames;
} acq_status_t;

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
/* Summary of the last (or current) session. */
const acq_summary_t *acq_summary(void);

#endif
