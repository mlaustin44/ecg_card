#include "acq.h"

#include <string.h>

#include "acq_stats.h"
#include "board.h"
#include "ecg_dsp.h"
#include "hal.h"
#include "rails.h"
#include "settings.h"

#define ECG_BLOCK_N     10u   /* STREAM_ECG_SAMPLES_PER_BLOCK */
#define PPG_BLOCK_N     2u    /* STREAM_PPG_FRAMES_PER_BLOCK */
#define ECG_CHUNK       64u   /* raw samples per hal_ecg_read() */
#define ECG_MAX_CHUNKS  32u   /* per poll: 2048 samples = 512 ms of backlog */
#define PPG_CHUNK       8u
#define PPG_MAX_CHUNKS  8u
/* Lost samples / frames are re-inserted by timeline index (hal.h) up to
 * 2.5 s per gap, longer than the watchdog lets the main loop stall; beyond
 * that the index is resynchronised (counted in acq_summary_t.*_resyncs). */
#define FILL_MAX_ECG    10000u
#define FILL_MAX_PPG    250u
#define GAIN_TICK_MS    100u
#define QUALITY_MS      1000u
#define SLOW_APPLY_MS   2u    /* a control write that took longer flags 2 frames */
#define DCC_FAST_FRAMES 25u   /* DC-cancel period during the first 2 s */
#define DCC_FAST_SPAN   200u  /* frames after start / finger with fast DCC */
#define AGC_QUIET       5u    /* frames after a step before the next AGC decision */

enum { S_NONE, S_VMID, S_GO };

_Static_assert((uint64_t)(DSP_PAT_MAX_US / ACQ_PAT_UNIT_US + 1) * UINT16_MAX < UINT32_MAX,
               "PAT sum cannot overflow");

static struct {
    const acq_sink_t *sink;
    acq_cfg_t cfg;
    acq_status_t st;
    acq_summary_t sum;
    uint8_t step;
    uint32_t step_ms;
    uint32_t start_ms;   /* hal_millis() right after hal_acq_start() */
    uint32_t last_ms;    /* previous poll */
    int gain;            /* 558 / 1098 */

    /* ---- ECG ---- */
    dsp_lead_t lead;
    dsp_decim_t decim;
    dsp_disp_t disp;
    dsp_mains_t mains;
    dsp_wander_t wander;
    dsp_qrs_t qrs;
    dsp_hrv_t hrv;
    dsp_gain_t gadv;
    int16_t blk[ECG_BLOCK_N];
    uint8_t blk_n;
    uint32_t blk_start;
    uint32_t ecg_next;   /* HAL timeline index of the next raw sample expected */
    uint16_t last_raw;
    bool have_raw;
    uint8_t ecg_state;   /* acq_ecg_state_t */
    bool ran;            /* RUN was reached this session */
    uint8_t lead_ev;     /* DSP_LEAD_EV_* since the last ecg_timers() */
    uint32_t stable_ms;      /* last stable edge */
    uint32_t last_stable_ms; /* last poll with a stable signal */
    uint32_t inrange_ms;     /* the output last came back in range (EV_INRANGE) */
    uint32_t settle_acc_ms;  /* SETTLE: in-range time accumulated */
    bool settle_done;
    uint32_t settle_done_ms; /* settling time reached */
    bool in_blip;            /* RUN: the current rail run was counted as a blip */
    bool have_beat;          /* an accepted beat since the contact began */
    uint32_t beat_ms;        /* ... the last one */
    uint32_t nobeat_ms;      /* in-range time since the last accepted beat (or since
                              * settling completed): rail time does not count */
    uint16_t qrs_amp[5];     /* uV on the display path, last accepted beats */
    uint8_t qrs_n, qrs_i;
    int16_t duv[256];        /* display-path uV by stream index (QRS heights as drawn) */
    /* auto-gain window (RUN only) */
    uint32_t gw_ms;          /* RUN time in the window */
    uint16_t gw_min, gw_max; /* raw codes */
    uint8_t gw_beats;
    bool gw_rail;            /* a rail sample in the window */
    bool gain_locked;        /* stepped down: stays low */
    bool fr_on, fr_any, fr_recheck;
    uint32_t fr_off_ms, fr_last_ms;
    bool blank_hold;         /* FAST_RESTORE held: every sample is blanked */
    uint32_t blank_until;    /* raw index: samples before it are blanked */
    uint32_t gain_ms, qual_ms;
    acq_diag_t diag;

    /* ---- PPG ---- */
    dsp_ppg_t ppg;
    dsp_finger_t finger;
    dsp_agc_t agc;
    dsp_pat_t pat;
    uint16_t led, led_start, led_max, dac;
    uint8_t pending, pending2;   /* DSP_PPG_IN_* for the next two frames */
    bool dcc_init;               /* first frame seen: cancel the offset from it */
    uint16_t dcc_init_tia;
    bool led_reset;              /* finger gone: back to the start current */
    uint32_t agc_on_sum, agc_dark_sum;
    uint16_t agc_n;
    uint32_t agc_k, dcc_k, step_k, fast_dcc_until;
    int16_t pblk[2 * PPG_BLOCK_N];
    uint8_t pblk_n, pblk_flags;
    uint16_t pblk_led;
    uint32_t pblk_start;
    uint32_t ppg_next;   /* HAL timeline index of the next frame expected */
    hal_ppg_frame_t last_frame;
    bool have_frame;
    uint32_t pulse_k;            /* ppg_frames at the last pulse (or finger edge) */

    /* ---- session statistics (settled only) ---- */
    acq_mean_t ibi, pi, pat_mean; /* saturating (acq_stats.h) */
    uint16_t pulses;
} A;

static void gain_window_reset(void);

/* Scratch buffers (static: keeps the main-loop stack shallow). */
static uint16_t s_raw[ECG_CHUNK];
static uint16_t s_fill[ECG_CHUNK];
static int16_t s_dec[DSP_DECIM_MAX_OUT(ECG_CHUNK)];
static int16_t s_uv[DSP_DECIM_MAX_OUT(ECG_CHUNK)];
static hal_ppg_frame_t s_frames[PPG_CHUNK];

/* ======================================================================== */
/* small helpers                                                             */
/* ======================================================================== */

void acq_init(const acq_sink_t *sink)
{
    memset(&A, 0, sizeof A);
    A.sink = sink;
    A.st.phase = ACQ_IDLE;
}

bool acq_active(void) { return A.st.phase == ACQ_STARTING || A.st.phase == ACQ_ON; }
const acq_status_t *acq_status(void) { return &A.st; }

static void diag_clear(void)
{
    memset(&A.diag, 0, sizeof A.diag);
    A.diag.hi_min = UINT16_MAX;
}

void acq_diag_take(acq_diag_t *d)
{
    if (A.cfg.ecg)
        dsp_lead_take_stats(&A.lead, &A.diag.ecg);
    else
        memset(&A.diag.ecg, 0, sizeof A.diag.ecg);
    *d = A.diag;
    diag_clear();
}

static void count16(uint16_t *c)
{
    if (*c < UINT16_MAX)
        (*c)++;
}
const acq_summary_t *acq_summary(void) { return &A.sum; }

static uint8_t notch_of(uint8_t mode)
{
    return mode == NOTCH_50 ? 50u : 60u; /* AUTO: 60 until the detector decides */
}

static bool ppg_primary(void) { return A.cfg.ppg && !A.cfg.ecg; }

static void set_contact(bool on, uint32_t now)
{
    A.st.contact = on;
    A.st.contact_ms = now;
    A.st.settle_pct = 0;
    if (A.sink && A.sink->contact)
        A.sink->contact(on);
}

static void set_settled(bool on)
{
    if (A.st.settled == on)
        return;
    A.st.settled = on;
    if (on)
        A.st.settle_pct = 100;
    if (A.sink && A.sink->settled)
        A.sink->settled(on);
}

static void fail(acq_err_t err)
{
    rails_off();
    A.st.phase = ACQ_FAILED;
    A.st.err = err;
    A.step = S_NONE;
}

/* ======================================================================== */
/* start sequence                                                            */
/* ======================================================================== */

bool acq_start(const acq_cfg_t *cfg)
{
    if (acq_active() || !cfg || (!cfg->ecg && !cfg->ppg))
        return false;
    const acq_sink_t *sink = A.sink;
    memset(&A, 0, sizeof A);
    A.sink = sink;
    A.cfg = *cfg;
    if (A.cfg.led_limit_ma > LED_MAX_MA)
        A.cfg.led_limit_ma = LED_MAX_MA;
    if (A.cfg.gain_auto)
        A.cfg.gain_hi = false; /* auto-gain starts low */
    A.gain = A.cfg.gain_hi ? ECG_GAIN_HI : ECG_GAIN_LO;
    A.st.phase = ACQ_STARTING;
    A.st.gain_hi = A.cfg.ecg && A.cfg.gain_hi;
    gain_window_reset();
    A.st.notch_hz = cfg->ecg ? notch_of(cfg->notch) : 0u;
    A.led_max = dsp_led_max_code(A.cfg.led_limit_ma);
    A.led_start = dsp_led_max_code(LED_START_MA);
    if (A.led_start > A.led_max)
        A.led_start = A.led_max;
    /* Rough initial offset for a mid-range light level; the first frame
     * replaces it (dcc_init). */
    A.dac = dsp_ppg_dcc_code(DSP_AGC_TGT_LO);
    A.ecg_state = ACQ_ECG_WAIT;
    diag_clear();

    /* ECG start step 2 / PPG start steps 1-3 */
    rails_analog_on();
    if (cfg->ppg) {
        if (!rails_ppg_analog_on(A.dac) || !rails_vled(true)) {
            fail(ACQ_ERR_START);
            return true;
        }
    }
    A.step = S_VMID;
    A.step_ms = hal_millis();
    return true;
}

static void dsp_setup(void)
{
    if (A.cfg.ecg) {
        uint16_t mid = A.st.mid_code;
        dsp_lead_init(&A.lead, mid);
        A.st.railed = dsp_lead_railed(&A.lead); /* no evidence yet */
        dsp_decim_init(&A.decim, mid);
        dsp_disp_init(&A.disp, A.gain, A.st.notch_hz);
        dsp_mains_init(&A.mains, A.gain);
        dsp_wander_init(&A.wander, A.gain);
        dsp_qrs_init(&A.qrs, 0);
        dsp_hrv_init(&A.hrv);
        dsp_gain_init(&A.gadv, A.cfg.gain_hi);
    }
    if (A.cfg.ppg) {
        dsp_ppg_init(&A.ppg);
        dsp_finger_init(&A.finger);
        dsp_agc_init(&A.agc);
        dsp_pat_init(&A.pat);
    }
}

static void start_step(uint32_t now)
{
    uint32_t dt = now - A.step_ms;
    switch (A.step) {
    case S_VMID:
        if (A.cfg.ecg) {
            /* step 3: VMID settles (50 ms time constant) */
            if (dt < ACQ_VMID_WAIT_MS)
                return;
            uint16_t code = hal_adc_read_once(HAL_ADC_VMID);
            uint32_t mv = ((uint32_t)code * ADC_UV_PER_CODE_X10 + 5000u) / 10000u;
            A.st.vmid_mv = (uint16_t)mv;
            A.st.mid_code = code;
            int32_t err = (int32_t)mv - (int32_t)(VDDA_MV / 2);
            if (err > ECG_VMID_TOL_MV || err < -ECG_VMID_TOL_MV) {
                fail(ACQ_ERR_VMID);
                return;
            }
            /* step 4: gain and right-leg drive; FAST_RESTORE stays low (the
             * brief). The charge the switches inject is removed by the
             * contact-detection pulse (step 7, ecg_on_contact()): until
             * contact the output sits on a rail anyway. */
            if (!rails_ecg_frontend(A.cfg.gain_hi, A.cfg.rld)) {
                fail(ACQ_ERR_START);
                return;
            }
            /* step 5: connect the electrodes (refused on USB power) */
            if (!rails_measure_req(true)) {
                fail(ACQ_ERR_USB);
                return;
            }
            A.step = S_GO;
            A.step_ms = now;
            start_step(now);
        } else {
            if (dt < ACQ_VLED_WAIT_MS)
                return;
            A.step = S_GO;
            start_step(now);
        }
        return;
    case S_GO: {
        if (A.cfg.ppg) {
            /* PPG step 4: channel A on at the start current (LEDs still
             * blanked: LED_BLANK is high until TIMA0 runs) */
            if (!rails_led_set(A.led_start)) {
                fail(ACQ_ERR_LED_DAC);
                return;
            }
            A.led = A.led_start;
        }
        dsp_setup();
        unsigned streams = (A.cfg.ecg ? HAL_ACQ_ECG : 0u) | (A.cfg.ppg ? HAL_ACQ_PPG : 0u);
        /* ECG step 6 / PPG step 5: one timer event starts every stream */
        if (!rails_acq_start(streams)) {
            fail(ACQ_ERR_START);
            return;
        }
        A.start_ms = hal_millis();
        A.last_ms = A.start_ms;
        A.ecg_next = 0;  /* hal.h: indices restart at hal_acq_start() */
        A.ppg_next = 0;
        A.gain_ms = A.qual_ms = A.start_ms;
        A.fast_dcc_until = DCC_FAST_SPAN;
        A.st.phase = ACQ_ON;
        A.step = S_NONE;
        A.st.led_code = A.led;
        A.st.dac12 = A.cfg.ppg ? A.dac : 0u;
        return;
    }
    default:
        return;
    }
}

/* ======================================================================== */
/* ECG                                                                       */
/* ======================================================================== */

static bool ecg_contact(void)
{
    return A.ecg_state == ACQ_ECG_SETTLE || A.ecg_state == ACQ_ECG_RUN ||
           A.ecg_state == ACQ_ECG_HOLD;
}

/* Raw timeline index of the sample being taken now (1 ms clock: up to 4
 * samples early). */
static uint32_t raw_index_now(uint32_t now)
{
    return (now - A.start_ms) * (ECG_RAW_HZ / 1000u);
}

static void fast_restore_pulse(uint32_t now)
{
    if (rails_fast_restore(true)) {
        A.fr_on = true;
        A.fr_any = true;
        A.fr_off_ms = now + ECG_FAST_RESTORE_MS;
        A.fr_last_ms = now;
        A.blank_hold = true; /* every sample from now on, until the release */
        count16(&A.diag.fr_pulses);
        A.sum.fr_pulses++;
    }
}

static void ecg_state_to(acq_ecg_state_t s, acq_why_t why)
{
    A.ecg_state = (uint8_t)s;
    A.st.ecg_state = (uint8_t)s;
    A.st.ecg_why = (uint8_t)why;
}

/* WAIT -> SETTLE: the signal is stable (the QRS detector has been learning
 * since it came back in range). */
static void ecg_on_contact(uint32_t now)
{
    ecg_state_to(ACQ_ECG_SETTLE, ACQ_WHY_STABLE);
    A.st.contact_beat = false;
    /* the automatic notch choice is made on contact data, not on the
     * floating inputs */
    dsp_mains_init(&A.mains, A.gain);
    if (A.cfg.ppg)
        dsp_pat_init(&A.pat);
    A.st.hr_bpm = 0;
    set_contact(true, now);
}

/* -> WAIT (never ran) or END (ran): contact is over. */
static void ecg_on_lead_off(acq_ecg_state_t s, acq_why_t why, uint32_t now)
{
    ecg_state_to(s, why);
    set_settled(false);
    A.st.hr_bpm = 0;
    A.st.quality = 0;
    set_contact(false, now);
}

/* ACQ_ECG_MIN_QRS_UV in stream units (8 per raw code). */
static uint32_t min_qrs_amp(void)
{
    return ACQ_ECG_MIN_QRS_UV * (uint32_t)A.gain * 8u * 10u / ADC_UV_PER_CODE_X10;
}

/* QRS peak-to-peak as drawn: the display-path samples within +-50 ms of the
 * R-peak (shifted by the display filter delay). 0 if not available. */
static uint16_t qrs_display_uv(uint32_t r_index)
{
    uint32_t c = r_index + (DSP_DISP_DELAY_Q8 + 128u) / 256u;
    uint32_t have = A.st.ecg_samples; /* display samples stored up to here */
    if (c + 25u >= have || have - (c - 25u) > 256u || c < 25u)
        return 0;
    int32_t lo = INT16_MAX, hi = INT16_MIN;
    for (uint32_t k = c - 25u; k <= c + 25u; k++) {
        int32_t v = A.duv[k & 255u];
        if (v < lo)
            lo = v;
        if (v > hi)
            hi = v;
    }
    return (uint16_t)(hi - lo > UINT16_MAX ? UINT16_MAX : hi - lo);
}

/* Median QRS of the last accepted beats, in uV (display scaling). */
static void qrs_amp_add(uint16_t amp)
{
    A.qrs_amp[A.qrs_i] = amp;
    A.qrs_i = (uint8_t)((A.qrs_i + 1u) % 5u);
    if (A.qrs_n < 5u)
        A.qrs_n++;
    uint16_t v[5];
    uint8_t n = A.qrs_n;
    for (uint8_t i = 0; i < n; i++) { /* insertion sort, 5 values */
        uint16_t x = A.qrs_amp[i];
        uint8_t j = i;
        while (j > 0 && v[j - 1u] > x) {
            v[j] = v[j - 1u];
            j--;
        }
        v[j] = x;
    }
    A.st.qrs_uv = v[n / 2u];
    A.st.qrs_neg = A.qrs.pol < 0;
}

static void ecg_beat(const dsp_qrs_beat_t *b, uint32_t now)
{
    bool good = b->amp >= min_qrs_amp();
    if (good) {
        A.have_beat = true;
        A.beat_ms = now;
        A.nobeat_ms = 0;
        uint16_t d = qrs_display_uv(b->index);
        if (d)
            qrs_amp_add(d);
        if (A.ecg_state == ACQ_ECG_RUN && A.gw_beats < 255u)
            A.gw_beats++;
    }
    if (!ecg_contact())
        return; /* evidence only: not reported while waiting */
    if (good)
        A.st.contact_beat = true;
    A.st.beat_seq++;
    count16(&A.st.ecg_beats);
    if (b->hr_bpm) /* 0 while the detector has fewer than 6 beats (after a gain switch) */
        A.st.hr_bpm = b->hr_bpm;
    if (A.st.settled) {
        dsp_hrv_add_beat(&A.hrv, b->rr_ms);
        dsp_gain_beat(&A.gadv, b->amp);
    }
    if (A.cfg.ppg)
        dsp_pat_add_r(&A.pat, b->index, b->frac_q8);
    if (A.sink && A.sink->beat)
        A.sink->beat(b->index, b->hr_bpm);
}

/* Stream block flags: bit1 settling (SETTLE / HOLD), bit2 lead-off (no
 * contact, or the output on a rail). */
static uint8_t ecg_flags(void)
{
    bool settling = A.ecg_state == ACQ_ECG_SETTLE || A.ecg_state == ACQ_ECG_HOLD;
    return (uint8_t)((A.st.gain_hi ? 1u : 0u) | (settling ? 2u : 0u) |
                     (!ecg_contact() || dsp_lead_railed(&A.lead) ? 4u : 0u));
}

/* raw[0] has timeline index idx. */
static void ecg_process(const uint16_t *raw, size_t n, uint32_t idx, uint32_t now)
{
    size_t nb = 0;
    if (A.blank_hold) {
        nb = n;
    } else {
        int32_t left = (int32_t)(A.blank_until - idx);
        if (left > 0)
            nb = (size_t)left < n ? (size_t)left : n;
    }
    uint8_t ev = dsp_lead_push(&A.lead, raw, n, nb);
    A.lead_ev |= ev;
    if (ev & DSP_LEAD_EV_INRANGE) {
        /* back in range: the display filter restarts without a step; the QRS
         * detector learns afresh while waiting for contact, and otherwise
         * resumes with its thresholds (dsp_qrs_skip() over the rail) */
        A.inrange_ms = now;
        dsp_disp_reset(&A.disp);
        if (A.ecg_state == ACQ_ECG_WAIT) {
            dsp_qrs_reset(&A.qrs);
            A.have_beat = false;
            A.st.hr_bpm = 0;
        }
    }
    A.last_raw = raw[n - 1];
    A.have_raw = true;
    A.st.railed = dsp_lead_railed(&A.lead);
    if (A.cfg.gain_auto && A.ecg_state == ACQ_ECG_RUN) {
        for (size_t i = nb; i < n; i++) {
            uint16_t c = raw[i];
            if (c < A.gw_min)
                A.gw_min = c;
            if (c > A.gw_max)
                A.gw_max = c;
            if (c < DSP_LEAD_RAIL_LO || c >= DSP_LEAD_RAIL_HI)
                A.gw_rail = true;
        }
    }

    size_t m = dsp_decim_push(&A.decim, raw, n, s_dec);
    if (m == 0)
        return;
    for (size_t i = 0; i < m; i++) {
        if (A.blk_n == 0)
            A.blk_start = A.st.ecg_samples + (uint32_t)i;
        A.blk[A.blk_n++] = s_dec[i];
        if (A.blk_n == ECG_BLOCK_N) {
            if (A.sink && A.sink->ecg_block)
                A.sink->ecg_block(A.blk_start, ecg_flags(), A.blk);
            A.blk_n = 0;
        }
    }
    A.st.ecg_samples += (uint32_t)m;

    if (dsp_mains_push(&A.mains, s_dec, m)) {
        A.st.mains_uvpp = dsp_mains_residual_uvpp(&A.mains);
        /* notch AUTO: follow the detector while settling on a stable signal */
        uint8_t hz = dsp_mains_auto_hz(&A.mains);
        if (A.cfg.notch == NOTCH_AUTO && ecg_contact() && !A.st.settled &&
            dsp_lead_stable(&A.lead) && hz && hz != A.st.notch_hz) {
            dsp_disp_set_notch(&A.disp, hz);
            A.st.notch_hz = hz;
        }
    }
    dsp_wander_push(&A.wander, s_dec, m);

    if (!dsp_lead_railed(&A.lead) && A.ecg_state != ACQ_ECG_END) {
        dsp_qrs_beat_t beats[2];
        size_t nb2 = dsp_qrs_process(&A.qrs, s_dec, m, beats, 2);
        for (size_t i = 0; i < nb2; i++)
            ecg_beat(&beats[i], now);
        dsp_disp_process(&A.disp, s_dec, s_uv, m);
        uint32_t i0 = A.st.ecg_samples - (uint32_t)m;
        for (size_t i = 0; i < m; i++)
            A.duv[(i0 + i) & 255u] = s_uv[i];
        if (A.st.settled && A.sink && A.sink->ecg_trace)
            A.sink->ecg_trace(s_uv, m);
    } else {
        dsp_qrs_skip(&A.qrs, (uint32_t)m);
        if (A.st.settled && A.sink && A.sink->ecg_gap)
            A.sink->ecg_gap(m); /* a blip: the sweep keeps time */
    }
}

static void count_resync(uint16_t *c)
{
    if (*c < UINT16_MAX)
        (*c)++;
}

/* Lost samples (slow reader, missed conversion) are re-inserted as copies of
 * the last sample, at the index where they belong (hal.h read contract), so
 * that every stream index stays on the acquisition timeline (R-peak times,
 * PAT). */
static void ecg_fill(uint32_t lost, uint16_t filler, uint32_t idx, uint32_t now)
{
    A.sum.ecg_lost += lost;
    if (lost > FILL_MAX_ECG) {
        count_resync(&A.sum.ecg_resyncs);
        lost = FILL_MAX_ECG;
    }
    while (lost) {
        size_t k = lost < ECG_CHUNK ? lost : ECG_CHUNK;
        for (size_t i = 0; i < k; i++)
            s_fill[i] = filler;
        ecg_process(s_fill, k, idx, now);
        idx += (uint32_t)k;
        lost -= (uint32_t)k;
    }
}

static void ecg_drain(uint32_t now)
{
    for (unsigned c = 0; c < ECG_MAX_CHUNKS; c++) {
        uint32_t first;
        size_t n = hal_ecg_read(s_raw, ECG_CHUNK, &first);
        if (n == 0)
            break;
        int32_t gap = (int32_t)(first - A.ecg_next);
        if (gap > 0) {
            ecg_fill((uint32_t)gap, A.have_raw ? A.last_raw : s_raw[0], A.ecg_next, now);
            A.ecg_next = first;
        } else if (gap < -(int32_t)FILL_MAX_ECG) {
            count_resync(&A.sum.ecg_resyncs); /* wild index: follow it */
            A.ecg_next = first;
        }
        /* overlap (only after a HAL ring resync): drop what was processed */
        uint32_t skip = A.ecg_next - first;
        if (skip >= n)
            continue;
        ecg_process(s_raw + skip, n - skip, first + skip, now);
        A.ecg_next = first + (uint32_t)n;
    }
}

static void ecg_to_run(void)
{
    A.ran = true;
    A.in_blip = false;
    ecg_state_to(ACQ_ECG_RUN, ACQ_WHY_SETTLED);
    set_settled(true);
}

/* The contact state machine (acq.h), once per poll on the detector state;
 * dt = ms since the previous poll. */
static void ecg_contact_update(uint32_t now, uint8_t ev, uint32_t dt)
{
    bool stable = dsp_lead_stable(&A.lead);
    bool railed = dsp_lead_railed(&A.lead);
    bool stable_edge = (ev & DSP_LEAD_EV_STABLE) && stable;
    if (stable_edge) {
        A.stable_ms = now;
        A.fr_recheck = true;
    }
    if (!stable)
        A.fr_recheck = false;
    else
        A.last_stable_ms = now;
    uint32_t rail_ms = dsp_lead_rail_samples(&A.lead) / (ECG_RAW_HZ / 1000u);
    /* floating inputs make short in-range excursions that restart a rail
     * run, so loss of contact is judged on the time without a stable signal */
    uint32_t unstable_ms = now - A.last_stable_ms;
    if (!railed)
        A.nobeat_ms += dt; /* a signal is there: beats should be in it */
    A.st.blip = false;
    switch (A.ecg_state) {
    case ACQ_ECG_WAIT:
        if (stable_edge) {
            ecg_on_contact(now);
            A.last_stable_ms = now;
            /* the in-range run that made it stable counts already */
            A.settle_acc_ms = dsp_lead_in_samples(&A.lead) / (ECG_RAW_HZ / 1000u);
            A.settle_done = false;
        }
        break;
    case ACQ_ECG_SETTLE:
        if (!stable && unstable_ms >= ACQ_ECG_LOST_WAIT_MS) {
            ecg_on_lead_off(ACQ_ECG_WAIT, ACQ_WHY_NO_STABLE, now);
            break;
        }
        if (railed) {
            if (rail_ms >= ACQ_ECG_BLIP_MS) {
                A.settle_acc_ms = 0; /* a long excursion: settle again */
                A.settle_done = false;
            }
            break; /* a short one only pauses the settling */
        }
        A.settle_acc_ms += dt;
        if (A.settle_acc_ms < ECG_SETTLE_MS)
            break;
        if (!A.settle_done) {
            A.settle_done = true;
            A.settle_done_ms = now;
            if (!A.have_beat)
                A.nobeat_ms = 0;
        }
        if (A.have_beat && A.nobeat_ms < ACQ_ECG_NO_BEAT_MS) {
            ecg_to_run();
        } else if (A.nobeat_ms >= ACQ_ECG_NO_BEAT_MS) {
            /* settled, in range for ACQ_ECG_NO_BEAT_MS (rail time not
             * counted) without a beat: open inputs, not a person */
            ecg_on_lead_off(ACQ_ECG_WAIT, ACQ_WHY_NO_BEAT, now);
        }
        break;
    case ACQ_ECG_RUN:
        if (rail_ms >= ACQ_ECG_BLIP_MS ||
            (!stable && unstable_ms >= ACQ_ECG_BLIP_MS + DSP_LEAD_STABLE_MS)) {
            ecg_state_to(ACQ_ECG_HOLD, ACQ_WHY_RAIL);
            set_settled(false);
            A.st.hr_bpm = 0;
            break;
        }
        if (railed) {
            A.st.blip = true;
            if (!A.in_blip) {
                A.in_blip = true;
                count16(&A.st.blips);
            }
            break;
        }
        A.in_blip = false;
        if (A.nobeat_ms >= ACQ_ECG_NO_BEAT_MS)
            ecg_on_lead_off(ACQ_ECG_END, ACQ_WHY_NO_BEAT, now);
        break;
    case ACQ_ECG_HOLD:
        if (!stable && unstable_ms >= ACQ_ECG_LOST_END_MS) {
            ecg_on_lead_off(ACQ_ECG_END, ACQ_WHY_NO_STABLE, now);
            break;
        }
        if (railed)
            break;
        if (now - A.beat_ms >= ACQ_ECG_HOLD_END_MS)
            ecg_on_lead_off(ACQ_ECG_END, ACQ_WHY_NO_BEAT, now);
        else if (now - A.inrange_ms >= ACQ_ECG_RESUME_MS && (int32_t)(A.beat_ms - A.inrange_ms) >= 0)
            ecg_to_run();
        break;
    default:
        break;
    }
    if (A.ecg_state == ACQ_ECG_SETTLE) {
        uint32_t pct = A.settle_acc_ms * 100u / ECG_SETTLE_MS;
        A.st.settle_pct = (uint8_t)(pct > 99u ? 99u : pct);
    } else if (A.ecg_state == ACQ_ECG_HOLD) {
        uint32_t pct = railed ? 0u : (now - A.inrange_ms) * 100u / ACQ_ECG_RESUME_MS;
        A.st.settle_pct = (uint8_t)(pct > 99u ? 99u : pct);
    }
}

static void ecg_fast_restore(uint32_t now)
{
    if (A.fr_on || A.ecg_state == ACQ_ECG_END)
        return;
    if (A.cfg.restore == RESTORE_OFF)
        return;
    bool slow = A.cfg.restore == RESTORE_SLOW;
    uint32_t rail_min = slow ? ACQ_FR_SLOW_RAIL_MS : ACQ_FR_RAIL_MS;
    uint32_t repeat = slow ? ACQ_FR_SLOW_REPEAT_MS : ACQ_FR_REPEAT_MS;
    uint32_t rail_ms = dsp_lead_rail_samples(&A.lead) / (ECG_RAW_HZ / 1000u);
    if (dsp_lead_railed(&A.lead)) {
        if (rail_ms >= rail_min && (!A.fr_any || now - A.fr_last_ms >= repeat))
            fast_restore_pulse(now);
    } else if (!slow && A.fr_recheck && now - A.stable_ms >= ACQ_FR_RECHECK_MS) {
        A.fr_recheck = false;
        if (dsp_lead_far_from_mid(&A.lead))
            fast_restore_pulse(now);
    }
}

static void gain_window_reset(void)
{
    A.gw_ms = 0;
    A.gw_min = UINT16_MAX;
    A.gw_max = 0;
    A.gw_beats = 0;
    A.gw_rail = false;
}

/* Switch the front-end gain within the session (auto-gain). */
static void ecg_set_gain(bool hi, uint32_t now)
{
    if (!rails_ecg_frontend(hi, A.cfg.rld))
        return;
    A.gain = hi ? ECG_GAIN_HI : ECG_GAIN_LO;
    A.st.gain_hi = hi;
    if (A.st.gain_switches < UINT8_MAX)
        A.st.gain_switches++;
    /* gain-dependent DSP state: the display and mains / wander scales, the
     * QRS thresholds (re-learn) and the QRS heights for the display scale */
    dsp_disp_init(&A.disp, A.gain, A.st.notch_hz);
    dsp_mains_init(&A.mains, A.gain);
    dsp_wander_init(&A.wander, A.gain);
    dsp_qrs_reset(&A.qrs);
    dsp_gain_init(&A.gadv, hi);
    /* the QRS heights (display scale) are in uV: unchanged by the switch */
    /* the switch may inject a little charge: no evidence for a moment */
    A.blank_until = raw_index_now(now) + (ACQ_GAIN_BLANK_MS + 2u) * (ECG_RAW_HZ / 1000u);
    gain_window_reset();
}

/* Auto-gain decisions (acq.h), every poll. */
static void ecg_auto_gain(uint32_t now, uint32_t dt)
{
    if (!A.cfg.gain_auto)
        return;
    if (A.st.gain_hi) {
        /* down at once when the higher gain does not fit */
        bool out = A.ecg_state == ACQ_ECG_HOLD || (A.ecg_state == ACQ_ECG_RUN && A.st.blip);
        int32_t mid = (int32_t)A.st.mid_code;
        int32_t dev = 0;
        if (A.gw_max >= A.gw_min) {
            int32_t up = (int32_t)A.gw_max - mid, dn = mid - (int32_t)A.gw_min;
            dev = up > dn ? up : dn;
        }
        if (out || dev * 100 > (int32_t)(ADC_FULL_SCALE / 2) * (int32_t)ACQ_GAIN_DOWN_PCT) {
            A.gain_locked = true;
            ecg_set_gain(false, now);
            return;
        }
        if (A.ecg_state == ACQ_ECG_RUN && !A.st.blip) {
            A.gw_ms += dt;
            if (A.gw_ms >= ACQ_GAIN_WIN_MS)
                gain_window_reset();
        }
        return;
    }
    if (A.gain_locked || A.ecg_state != ACQ_ECG_RUN || A.st.blip) {
        if (A.ecg_state != ACQ_ECG_RUN || A.st.blip)
            gain_window_reset(); /* the window is RUN without interruption */
        return;
    }
    A.gw_ms += dt;
    if (A.gw_ms < ACQ_GAIN_WIN_MS)
        return;
    bool ok = A.gw_beats >= ACQ_GAIN_MIN_BEATS && !A.gw_rail && A.gw_max >= A.gw_min;
    if (ok) {
        int32_t mid = (int32_t)A.st.mid_code;
        int32_t up = (int32_t)A.gw_max - mid, dn = mid - (int32_t)A.gw_min;
        int32_t dev = up > dn ? up : dn;
        /* projected excursion at the higher gain, against ACQ_GAIN_UP_PCT % of half-scale */
        ok = (int64_t)dev * ECG_GAIN_HI * 100 <=
             (int64_t)(ADC_FULL_SCALE / 2) * ACQ_GAIN_UP_PCT * ECG_GAIN_LO;
    }
    if (ok)
        ecg_set_gain(true, now);
    else
        gain_window_reset();
}

static void ecg_timers(uint32_t now)
{
    if (A.fr_on && (int32_t)(now - A.fr_off_ms) >= 0) {
        rails_fast_restore(false);
        A.fr_on = false;
        A.blank_hold = false;
        /* + 2 ms for the 1 ms clock */
        A.blank_until = raw_index_now(now) + (DSP_LEAD_BLANK_MS + 2u) * (ECG_RAW_HZ / 1000u);
    }
    uint8_t ev = A.lead_ev;
    A.lead_ev = 0;
    ecg_contact_update(now, ev, now - A.last_ms);
    ecg_auto_gain(now, now - A.last_ms);
    ecg_fast_restore(now);
    if (now - A.gain_ms >= GAIN_TICK_MS) {
        uint32_t el = now - A.gain_ms;
        A.gain_ms = now;
        uint32_t clip = dsp_lead_take_clip(&A.lead);
        if (A.st.settled)
            dsp_gain_tick(&A.gadv, (uint16_t)(el > 1000u ? 1000u : el), clip);
    }
    if (now - A.qual_ms >= QUALITY_MS) {
        A.qual_ms = now;
        if (!ecg_contact()) {
            A.st.quality = 0;
        } else {
            dsp_quality_in_t q;
            uint32_t since_contact = now - A.st.contact_ms;
            uint32_t since_beat = dsp_qrs_since_beat(&A.qrs) * 2u;
            q.lead_off = dsp_lead_railed(&A.lead);
            q.rail_per_s = dsp_lead_rail_last_s(&A.lead);
            q.mains_uvpp = dsp_mains_residual_uvpp(&A.mains);
            q.wander_uvpp = dsp_wander_uvpp(&A.wander);
            q.rr_spread_pct = dsp_qrs_rr_spread_pct(&A.qrs);
            q.ms_since_beat = since_beat < since_contact ? since_beat : since_contact;
            q.ms_since_contact = since_contact;
            A.st.quality = dsp_ecg_quality(&q);
        }
    }
}

/* ======================================================================== */
/* PPG                                                                       */
/* ======================================================================== */

static void ppg_on_finger(bool present, uint32_t now)
{
    A.st.finger = present;
    A.pulse_k = A.st.ppg_frames;
    if (present) {
        dsp_ppg_reset(&A.ppg);
        A.fast_dcc_until = A.st.ppg_frames + DCC_FAST_SPAN;
    } else {
        A.st.pulse_bpm = 0;
        A.st.pat_ms = 0;
        A.st.pi_permille = 0;
        A.led_reset = true;
        dsp_agc_init(&A.agc);
    }
    if (ppg_primary()) {
        if (!present) {
            set_settled(false);
            A.st.hr_bpm = 0;
            A.st.quality = 0;
        }
        set_contact(present, now);
    }
}

static void ppg_pulse(const dsp_ppg_pulse_t *p)
{
    uint16_t pat_ms = 0;
    bool settled = A.st.settled && A.st.finger;
    A.pulse_k = A.st.ppg_frames;
    count16(&A.st.ppg_pulses);
    A.st.pulse_bpm = p->pulse_bpm;
    if (p->pi_permille)
        A.st.pi_permille = p->pi_permille;
    if (A.cfg.ecg) {
        uint32_t pat_us;
        dsp_pat_add_foot(&A.pat, p->frame, p->frac_q8);
        if (dsp_pat_pop(&A.pat, &pat_us, NULL)) {
            pat_ms = (uint16_t)((pat_us + 500u) / 1000u);
            A.st.pat_ms = pat_ms;
            if (settled)
                acq_pat_add(&A.pat_mean, pat_us);
        }
    } else {
        A.st.beat_seq++;
        A.st.hr_bpm = p->pulse_bpm;
    }
    if (settled) {
        if (A.pulses < UINT16_MAX)
            A.pulses++;
        if (p->ibi_ms >= 300u && p->ibi_ms <= 2500u)
            acq_mean_add(&A.ibi, p->ibi_ms);
        if (p->pi_permille)
            acq_mean_add(&A.pi, p->pi_permille);
    }
    if (A.sink && A.sink->pulse)
        A.sink->pulse(dsp_pat_foot_ecg_index(p->frame, p->frac_q8), p->pulse_bpm, pat_ms,
                      p->pi_permille);
}

static void ppg_frame(const hal_ppg_frame_t *f, uint8_t extra, uint32_t now)
{
    uint8_t in = (uint8_t)(A.pending | extra);
    A.pending = A.pending2;
    A.pending2 = 0;
    dsp_ppg_frame_t df = {f->tia_dark, f->hi_dark, f->tia_on, f->hi_on};
    dsp_ppg_out_t out;
    dsp_ppg_pulse_t pulse;
    bool got = dsp_ppg_process(&A.ppg, &df, A.dac, in, &out, &pulse);
    uint32_t k = A.st.ppg_frames++;
    A.step_k++;

    bool finger = dsp_finger_update(&A.finger, out.coarse, f->tia_on);
    if (!(extra & DSP_PPG_IN_DISTURBED) && A.diag.ppg_n < UINT16_MAX) { /* measured, not a filler */
        acq_diag_t *d = &A.diag;
        d->ppg_n++;
        d->dark_sum += f->tia_dark;
        d->on_sum += f->tia_on;
        d->hi_sum += f->hi_on;
        d->coarse_sum += (int32_t)f->tia_on - (int32_t)f->tia_dark;
        if (f->hi_on < d->hi_min)
            d->hi_min = f->hi_on;
        if (f->hi_on > d->hi_max)
            d->hi_max = f->hi_on;
    }

    if (A.pblk_n == 0) {
        A.pblk_start = k;
        A.pblk_led = A.led;
        A.pblk_flags = 0;
    }
    A.pblk[2u * A.pblk_n] = out.coarse;
    A.pblk[2u * A.pblk_n + 1u] = out.fine;
    if (in)
        A.pblk_flags |= (uint8_t)(1u << A.pblk_n);
    if (!finger)
        A.pblk_flags |= 4u;
    if (++A.pblk_n == PPG_BLOCK_N) {
        if (A.sink && A.sink->ppg_block)
            A.sink->ppg_block(A.pblk_start, A.pblk_led, A.pblk_flags, A.pblk);
        A.pblk_n = 0;
    }

    if (finger != A.st.finger)
        ppg_on_finger(finger, now);
    if (!in) {
        A.agc_on_sum += f->tia_on;
        A.agc_dark_sum += f->tia_dark;
        A.agc_n++;
    }
    if (!A.have_frame) {
        A.dcc_init = true;
        A.dcc_init_tia = f->tia_on;
    }
    A.have_frame = true;
    A.last_frame = *f;
    if (A.st.finger && A.st.settled && A.sink && A.sink->ppg_trace)
        A.sink->ppg_trace(&out.disp, 1);
    if (got)
        ppg_pulse(&pulse);
}

static void ppg_drain(uint32_t now)
{
    for (unsigned c = 0; c < PPG_MAX_CHUNKS; c++) {
        uint32_t first;
        size_t n = hal_ppg_read(s_frames, PPG_CHUNK, &first);
        if (n == 0)
            break;
        int32_t gap = (int32_t)(first - A.ppg_next);
        if (gap > 0) {
            /* keep the frame index on the timeline; the DSP bridges them */
            uint32_t lost = (uint32_t)gap;
            A.sum.ppg_lost += lost;
            if (lost > FILL_MAX_PPG) {
                count_resync(&A.sum.ppg_resyncs);
                lost = FILL_MAX_PPG;
            }
            hal_ppg_frame_t filler = A.have_frame ? A.last_frame : s_frames[0];
            for (uint32_t i = 0; i < lost; i++)
                ppg_frame(&filler, DSP_PPG_IN_DISTURBED, now);
            A.ppg_next = first;
        } else if (gap < -(int32_t)FILL_MAX_PPG) {
            count_resync(&A.sum.ppg_resyncs);
            A.ppg_next = first;
        }
        uint32_t skip = A.ppg_next - first; /* overlap after a HAL resync */
        for (size_t i = skip; i < n; i++)
            ppg_frame(&s_frames[i], 0, now);
        if (skip < n)
            A.ppg_next = first + (uint32_t)n;
    }
}

/* Mark the next frame(s) as disturbed after a control write. */
static void flag_step(uint8_t flags, uint32_t t0)
{
    A.pending |= flags;
    if (hal_millis() - t0 >= SLOW_APPLY_MS)
        A.pending2 |= flags; /* the write may have straddled a sample */
    A.step_k = 0;
}

static void set_dac(uint16_t code, uint32_t t0)
{
    if (code == A.dac)
        return;
    A.dac = code;
    rails_ppg_offset(code);
    A.st.dac12 = code;
    count16(&A.st.dcc_steps);
    flag_step(DSP_PPG_IN_DAC_STEP, t0);
}

static bool set_led(uint16_t code, uint32_t t0)
{
    if (code == A.led)
        return true;
    if (!rails_led_set(code))
        return false;
    A.led = code;
    A.st.led_code = code;
    flag_step(DSP_PPG_IN_LED_STEP, t0);
    return true;
}

/* Control changes land between frames: every complete frame has been
 * drained just before, and the time since the common start event puts us
 * 1..8 ms into the current frame, before its dark sample at 9.7 ms
 * (hal_millis() has 1 ms resolution, so 0 and 8-9 are excluded). */
static void ppg_control(uint32_t now)
{
    uint32_t phase = (now - A.start_ms) % 10u;
    if (phase < 1u || phase > 7u)
        return;
    uint32_t t0 = hal_millis();
    dsp_ppg_avg_t avg;

    if (A.dcc_init) {
        A.dcc_init = false;
        set_dac(dsp_ppg_dcc_code(A.dcc_init_tia), t0);
        (void)dsp_ppg_take_avg(&A.ppg, &avg); /* restart the averaging window */
        A.dcc_k = A.st.ppg_frames;
        return;
    }
    if (A.led_reset) {
        A.led_reset = false;
        if (A.led != A.led_start && set_led(A.led_start, t0)) {
            (void)dsp_ppg_take_avg(&A.ppg, &avg);
            A.dcc_k = A.st.ppg_frames - ACQ_DCC_FRAMES + DCC_FAST_FRAMES;
        }
        A.agc_on_sum = A.agc_dark_sum = 0;
        A.agc_n = 0;
        return;
    }
    if (A.st.ppg_frames - A.agc_k >= ACQ_AGC_FRAMES) {
        A.agc_k = A.st.ppg_frames;
        if (A.st.finger && A.agc_n && A.step_k > AGC_QUIET) {
            uint16_t tia = (uint16_t)(A.agc_on_sum / A.agc_n);
            uint16_t dark = (uint16_t)(A.agc_dark_sum / A.agc_n);
            uint16_t old = A.led, nc = A.led;
            if (dsp_agc_step(&A.agc, tia, &nc, A.led_max) && set_led(nc, t0)) {
                count16(&A.st.agc_steps);
                /* re-cancel the offset with the step so PPG_HI does not rail */
                set_dac(dsp_ppg_dcc_predict(dark, tia, old, nc), t0);
                (void)dsp_ppg_take_avg(&A.ppg, &avg);
                A.dcc_k = A.st.ppg_frames;
            }
        }
        A.agc_on_sum = A.agc_dark_sum = 0;
        A.agc_n = 0;
    }
    uint32_t period = A.st.ppg_frames < A.fast_dcc_until ? DCC_FAST_FRAMES : ACQ_DCC_FRAMES;
    if (A.st.ppg_frames - A.dcc_k >= period) {
        A.dcc_k = A.st.ppg_frames;
        uint16_t code = A.dac;
        if (dsp_ppg_take_avg(&A.ppg, &avg) && dsp_ppg_dcc_update(avg.tia_on, avg.hi_on, &code))
            set_dac(code, t0);
    }
}

/* No pulse for ACQ_PULSE_TIMEOUT_MS: the last rate, PI and PAT are stale.
 * Report 0 ("not available", the UI shows "--") until the detector delivers
 * again; in PPG-only mode the heart rate is the pulse rate. */
static void ppg_stale(void)
{
    if (A.st.ppg_frames - A.pulse_k < ACQ_PULSE_TIMEOUT_MS / (PPG_FRAME_US / 1000u))
        return;
    A.st.pulse_bpm = 0;
    A.st.pi_permille = 0;
    A.st.pat_ms = 0;
    if (ppg_primary())
        A.st.hr_bpm = 0;
}

static void ppg_timers(uint32_t now)
{
    ppg_stale();
    if (!ppg_primary())
        return;
    if (A.st.finger && !A.st.settled) {
        uint32_t dt = now - A.st.contact_ms;
        if (dt >= ACQ_PPG_SETTLE_MS)
            set_settled(true);
        else
            A.st.settle_pct = (uint8_t)(dt * 100u / ACQ_PPG_SETTLE_MS);
    }
    /* PPG quality: no finger 0, settling 2, then 3 / 4 by rate and PI */
    if (!A.st.finger)
        A.st.quality = 0;
    else if (!A.st.settled || A.st.pulse_bpm == 0)
        A.st.quality = 2;
    else
        A.st.quality = A.st.pi_permille >= 5u ? 4u : 3u;
}

/* ======================================================================== */
/* poll / stop                                                               */
/* ======================================================================== */

void acq_poll(uint32_t now)
{
    if (A.st.phase == ACQ_STARTING) {
        start_step(now);
        return;
    }
    if (A.st.phase != ACQ_ON)
        return;
    if (A.cfg.ecg) {
        ecg_drain(now);
        ecg_timers(now);
    }
    if (A.cfg.ppg) {
        ppg_drain(now);
        ppg_control(now);
        ppg_timers(now);
    }
    if (A.st.settled && !A.st.blip) /* a blip is not measured time */
        A.st.running_ms += now - A.last_ms;
    A.last_ms = now;
}

static void make_summary(void)
{
    acq_summary_t *s = &A.sum;
    if (A.cfg.ecg) {
        const dsp_lead_totals_t *t = dsp_lead_totals(&A.lead);
        s->ecg_raw_n = t->n;
        s->ecg_rail_n = t->rail;
        s->ecg_run_max_ms = t->run_max / (ECG_RAW_HZ / 1000u);
        dsp_hrv_summary(&A.hrv, &s->hrv);
        s->gain_advice = A.gadv.advice == DSP_GAIN_UP ? 1 : A.gadv.advice == DSP_GAIN_DOWN ? -1 : 0;
    } else {
        memset(&s->hrv, 0, sizeof s->hrv);
        s->gain_advice = 0;
    }
    s->pulses = A.pulses;
    s->pulse_avg = acq_mean_bpm(&A.ibi);
    s->pi_avg = (uint16_t)acq_mean_get(&A.pi);
    s->pat_n = A.pat_mean.n;
    s->pat_avg_ms = acq_pat_mean_ms(&A.pat_mean);
    s->running_ms = A.st.running_ms;
    s->i2c_errors = rails_i2c_errors();
    s->gain_hi = A.st.gain_hi;
    s->notch_hz = A.st.notch_hz;
    s->mid_code = A.st.mid_code;
    s->led_code = A.st.led_code;
    s->dac12 = A.st.dac12;
}

void acq_stop(void)
{
    bool was = acq_active();
    rails_off();
    if (!was)
        return;
    make_summary();
    set_settled(false);
    A.st.phase = ACQ_IDLE;
    A.step = S_NONE;
    A.st.contact = false;
    A.st.finger = false;
}
