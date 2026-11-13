#include "acq.h"

#include <string.h>

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
#define FILL_MAX_ECG    4000u /* lost samples re-inserted per poll (1 s) */
#define FILL_MAX_PPG    100u
#define GAIN_TICK_MS    100u
#define QUALITY_MS      1000u
#define SLOW_APPLY_MS   2u    /* a control write that took longer flags 2 frames */
#define DCC_FAST_FRAMES 25u   /* DC-cancel period during the first 2 s */
#define DCC_FAST_SPAN   200u  /* frames after start / finger with fast DCC */
#define AGC_QUIET       5u    /* frames after a step before the next AGC decision */

enum { S_NONE, S_VMID, S_FR, S_GO };

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
    uint32_t ecg_ovr0;
    uint16_t last_raw;
    bool ecg_contact;
    uint32_t ecg_contact_ms;
    bool fr_on, fr_recheck;
    uint32_t fr_off_ms;
    uint32_t gain_ms, qual_ms;

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
    uint32_t ppg_ovr0;
    hal_ppg_frame_t last_frame;
    bool have_frame;

    /* ---- session statistics (settled only) ---- */
    uint32_t ibi_sum;
    uint16_t ibi_n;
    uint32_t pi_sum;
    uint16_t pi_n;
    uint32_t pat_sum_us;
    uint16_t pat_n;
    uint16_t pulses;
} A;

/* Scratch buffers (static: keeps the main-loop stack shallow). */
static uint16_t s_raw[ECG_CHUNK];
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
    A.gain = cfg->gain_hi ? ECG_GAIN_HI : ECG_GAIN_LO;
    A.st.phase = ACQ_STARTING;
    A.st.gain_hi = cfg->ecg && cfg->gain_hi;
    A.st.notch_hz = cfg->ecg ? notch_of(cfg->notch) : 0u;
    A.led_max = dsp_led_max_code(A.cfg.led_limit_ma);
    A.led_start = dsp_led_max_code(LED_START_MA);
    if (A.led_start > A.led_max)
        A.led_start = A.led_max;
    /* Rough initial offset for a mid-range light level; the first frame
     * replaces it (dcc_init). */
    A.dac = dsp_ppg_dcc_code(DSP_AGC_TGT_LO);

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
            /* step 4: gain and right-leg drive, FAST_RESTORE after the
             * switch (it injects charge) */
            if (!rails_ecg_frontend(A.cfg.gain_hi, A.cfg.rld) || !rails_fast_restore(true)) {
                fail(ACQ_ERR_START);
                return;
            }
            A.step = S_FR;
            A.step_ms = now;
        } else {
            if (dt < ACQ_VLED_WAIT_MS)
                return;
            A.step = S_GO;
            start_step(now);
        }
        return;
    case S_FR:
        if (dt < ECG_FAST_RESTORE_MS)
            return;
        rails_fast_restore(false);
        /* step 5: connect the electrodes (refused on USB power) */
        if (!rails_measure_req(true)) {
            fail(ACQ_ERR_USB);
            return;
        }
        A.step = S_GO;
        /* fall through */
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
        A.ecg_ovr0 = hal_ecg_overruns();
        A.ppg_ovr0 = hal_ppg_overruns();
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

static void fast_restore_pulse(uint32_t now)
{
    if (rails_fast_restore(true)) {
        A.fr_on = true;
        A.fr_off_ms = now + ECG_FAST_RESTORE_MS;
    }
}

static void ecg_on_contact(uint32_t now)
{
    A.ecg_contact = true;
    A.ecg_contact_ms = now;
    fast_restore_pulse(now);
    A.fr_recheck = true;
    dsp_qrs_reset(&A.qrs);
    dsp_disp_reset(&A.disp);
    /* the automatic notch choice is made on contact data, not on the
     * floating lead-off signal */
    dsp_mains_init(&A.mains, A.gain);
    if (A.cfg.ppg)
        dsp_pat_init(&A.pat);
    A.st.hr_bpm = 0;
    set_contact(true, now);
}

static void ecg_on_lead_off(uint32_t now)
{
    A.ecg_contact = false;
    set_settled(false);
    A.st.hr_bpm = 0;
    A.st.quality = 0;
    set_contact(false, now);
}

static void ecg_beat(const dsp_qrs_beat_t *b)
{
    A.st.beat_seq++;
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

static uint8_t ecg_flags(void)
{
    return (uint8_t)((A.st.gain_hi ? 1u : 0u) |
                     (A.ecg_contact && !A.st.settled ? 2u : 0u) |
                     (dsp_lead_is_off(&A.lead) ? 4u : 0u));
}

static void ecg_process(const uint16_t *raw, size_t n, uint32_t now)
{
    uint8_t ev = dsp_lead_push(&A.lead, raw, n);
    if (ev) {
        bool off_now = dsp_lead_is_off(&A.lead);
        if ((ev & DSP_LEAD_EV_OFF) && (ev & DSP_LEAD_EV_CONTACT)) {
            /* both edges in one block: replay them in the order that ends in
             * the detector's current state */
            if (off_now) {
                ecg_on_contact(now);
                ecg_on_lead_off(now);
            } else {
                ecg_on_lead_off(now);
                ecg_on_contact(now);
            }
        } else if (ev & DSP_LEAD_EV_OFF) {
            ecg_on_lead_off(now);
        } else {
            ecg_on_contact(now);
        }
    }
    A.last_raw = raw[n - 1];

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
        /* notch AUTO: follow the detector while settling only */
        uint8_t hz = dsp_mains_auto_hz(&A.mains);
        if (A.cfg.notch == NOTCH_AUTO && A.ecg_contact && !A.st.settled && hz && hz != A.st.notch_hz) {
            dsp_disp_set_notch(&A.disp, hz);
            A.st.notch_hz = hz;
        }
    }
    dsp_wander_push(&A.wander, s_dec, m);

    if (A.ecg_contact) {
        dsp_qrs_beat_t beats[2];
        size_t nb = dsp_qrs_process(&A.qrs, s_dec, m, beats, 2);
        for (size_t i = 0; i < nb; i++)
            ecg_beat(&beats[i]);
        dsp_disp_process(&A.disp, s_dec, s_uv, m);
        if (A.st.settled && A.sink && A.sink->ecg_trace)
            A.sink->ecg_trace(s_uv, m);
    } else {
        dsp_qrs_skip(&A.qrs, (uint32_t)m);
    }
}

static void ecg_drain(uint32_t now)
{
    /* Lost samples (slow reader, missed conversion) are re-inserted as
     * copies of the last sample so that every stream index stays on the
     * acquisition timeline (R-peak times, PAT). */
    uint32_t ovr = hal_ecg_overruns();
    uint32_t lost = ovr - A.ecg_ovr0;
    A.ecg_ovr0 = ovr;
    if (lost) {
        A.sum.ecg_lost += lost;
        if (lost > FILL_MAX_ECG)
            lost = FILL_MAX_ECG;
        while (lost) {
            size_t k = lost < ECG_CHUNK ? lost : ECG_CHUNK;
            for (size_t i = 0; i < k; i++)
                s_raw[i] = A.last_raw;
            ecg_process(s_raw, k, now);
            lost -= (uint32_t)k;
        }
    }
    for (unsigned c = 0; c < ECG_MAX_CHUNKS; c++) {
        size_t n = hal_ecg_read(s_raw, ECG_CHUNK);
        if (n == 0)
            break;
        ecg_process(s_raw, n, now);
        if (n < ECG_CHUNK)
            break;
    }
}

static void ecg_timers(uint32_t now)
{
    if (A.fr_on && (int32_t)(now - A.fr_off_ms) >= 0) {
        rails_fast_restore(false);
        A.fr_on = false;
    }
    if (A.fr_recheck && A.ecg_contact && now - A.ecg_contact_ms >= ACQ_FR_RECHECK_MS) {
        A.fr_recheck = false;
        if (dsp_lead_far_from_mid(&A.lead) && !A.fr_on)
            fast_restore_pulse(now);
    }
    if (A.ecg_contact && !A.st.settled) {
        uint32_t dt = now - A.ecg_contact_ms;
        if (dt >= ECG_SETTLE_MS)
            set_settled(true);
        else
            A.st.settle_pct = (uint8_t)(dt * 100u / ECG_SETTLE_MS);
    }
    if (now - A.gain_ms >= GAIN_TICK_MS) {
        uint32_t el = now - A.gain_ms;
        A.gain_ms = now;
        uint32_t clip = dsp_lead_take_clip(&A.lead);
        if (A.st.settled)
            dsp_gain_tick(&A.gadv, (uint16_t)(el > 1000u ? 1000u : el), clip);
    }
    if (now - A.qual_ms >= QUALITY_MS) {
        A.qual_ms = now;
        if (!A.ecg_contact) {
            A.st.quality = 0;
        } else {
            dsp_quality_in_t q;
            uint32_t since_contact = now - A.ecg_contact_ms;
            uint32_t since_beat = dsp_qrs_since_beat(&A.qrs) * 2u;
            q.lead_off = dsp_lead_is_off(&A.lead);
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
    A.st.pulse_bpm = p->pulse_bpm;
    if (p->pi_permille)
        A.st.pi_permille = p->pi_permille;
    if (A.cfg.ecg) {
        uint32_t pat_us;
        dsp_pat_add_foot(&A.pat, p->frame, p->frac_q8);
        if (dsp_pat_pop(&A.pat, &pat_us, NULL)) {
            pat_ms = (uint16_t)((pat_us + 500u) / 1000u);
            A.st.pat_ms = pat_ms;
            if (settled) {
                A.pat_sum_us += pat_us;
                A.pat_n++;
            }
        }
    } else {
        A.st.beat_seq++;
        A.st.hr_bpm = p->pulse_bpm;
    }
    if (settled) {
        if (A.pulses < UINT16_MAX)
            A.pulses++;
        if (p->ibi_ms >= 300u && p->ibi_ms <= 2500u && A.ibi_n < UINT16_MAX) {
            A.ibi_sum += p->ibi_ms;
            A.ibi_n++;
        }
        if (p->pi_permille && A.pi_n < UINT16_MAX) {
            A.pi_sum += p->pi_permille;
            A.pi_n++;
        }
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
    uint32_t ovr = hal_ppg_overruns();
    uint32_t lost = ovr - A.ppg_ovr0;
    A.ppg_ovr0 = ovr;
    if (lost) {
        A.sum.ppg_lost += lost;
        if (lost > FILL_MAX_PPG)
            lost = FILL_MAX_PPG;
        /* keep the frame index on the timeline; the DSP bridges them */
        for (uint32_t i = 0; i < lost && A.have_frame; i++)
            ppg_frame(&A.last_frame, DSP_PPG_IN_DISTURBED, now);
    }
    for (unsigned c = 0; c < PPG_MAX_CHUNKS; c++) {
        size_t n = hal_ppg_read(s_frames, PPG_CHUNK);
        for (size_t i = 0; i < n; i++)
            ppg_frame(&s_frames[i], 0, now);
        if (n < PPG_CHUNK)
            break;
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

static void ppg_timers(uint32_t now)
{
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
    if (A.st.settled)
        A.st.running_ms += now - A.last_ms;
    A.last_ms = now;
}

static void make_summary(void)
{
    acq_summary_t *s = &A.sum;
    if (A.cfg.ecg) {
        dsp_hrv_summary(&A.hrv, &s->hrv);
        s->gain_advice = A.gadv.advice == DSP_GAIN_UP ? 1 : A.gadv.advice == DSP_GAIN_DOWN ? -1 : 0;
    } else {
        memset(&s->hrv, 0, sizeof s->hrv);
        s->gain_advice = 0;
    }
    s->pulses = A.pulses;
    s->pulse_avg = A.ibi_n ? (uint16_t)((60000u * A.ibi_n + A.ibi_sum / 2u) / A.ibi_sum) : 0u;
    s->pi_avg = A.pi_n ? (uint16_t)((A.pi_sum + A.pi_n / 2u) / A.pi_n) : 0u;
    s->pat_n = A.pat_n;
    s->pat_avg_ms = A.pat_n ? (uint16_t)((A.pat_sum_us / A.pat_n + 500u) / 1000u) : 0u;
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
