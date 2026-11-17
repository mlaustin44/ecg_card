/* Main loop and application state machine.
 *
 * One pass (app_step):
 *   1. events: USB plug first (end an ECG record at once), wake handling
 *   2. acq_poll()            drain the ECG / PPG rings, DSP, control loops
 *   3. buttons, charger      debounce, events -> navigation actions
 *   4. session supervisor    contact / settling / timeouts / summary
 *   5. battery
 *   6. radio link            UART RX, phone commands, link state machine
 *   7. power policy, settings save, diagnostics
 *   8. display               render + DMA flush at <= 25 fps, never while busy
 *   9. watchdog kick, hal_idle()
 * In the off state a pass is one hal_standby() plus its wake handling.
 *
 * Boot and power-down screens (UI_SCREEN_BOOT / UI_SCREEN_POWER_DOWN) credit
 * the designer. BOOT shows for BOOT_SCREEN_MS after a reset and after a
 * button wake from STANDBY (not after a USB or timed wake); everything boots
 * underneath it, a new button press skips it to Home and does nothing else,
 * and faults found at boot are shown after it. POWER_DOWN shows for
 * POWER_DOWN_SCREEN_MS after BUTTON1 long is released (acquisition and
 * analog rails already off; buttons ignored), then enter_standby() runs as
 * before (radio handshake, settings, display power-down).
 *
 * Nothing here blocks for more than a few ms while acquisition runs: the
 * display power-off (100 ms) and flash writes happen only with acquisition
 * stopped. The radio path never waits (radio.h); only STANDBY entry runs the
 * shutdown handshake inline (<= LINK_SHUTDOWN_WAIT_MS, acquisition stopped).
 *
 * Phone commands (LINK_CMD, radio.h) go through the same paths as the
 * buttons (start_session(), end_session(), the Settings validation and the
 * deferred flash save). Rules decided here:
 *   START  refused BUSY while a session is active (no restart: STOP first),
 *          while an overlay, Diagnostics, program mode or a pending power-off
 *          is on screen; REFUSED_BATT with a critical battery; otherwise the
 *          button rules: USB + ECG -> REFUSED_USB (the USB screen is shown,
 *          as for the button), battery low -> REFUSED_BATT, PPG without the
 *          LED DAC -> FAULT. OK means armed; a later analog self-check
 *          failure shows on the card and in LINK_STATUS.meas. The display
 *          wakes and shows the measurement screen.
 *   STOP   ends the session like BUTTON1 on the live screen (summary or
 *          card); OK also when nothing runs.
 *   SET    the Settings page values; gain, notch, RLD and LED limit are
 *          refused BUSY while a session is active (they are applied at
 *          start); sweep and Bluetooth may change any time. Saved to flash
 *          SETTINGS_SAVE_IDLE_MS after the last change, never during
 *          acquisition.
 *   SET_TIME  wall clock = unix + offset; kept through STANDBY on the RTC,
 *          lost on reset.
 * Timeouts: a phone command counts as input for the STANDBY timeout (not
 * for the display timeout): a connected client that polls GET_STATUS keeps
 * the card awake with the display off; a silent one lets it sleep after
 * UI_STANDBY_TIMEOUT_S (the shutdown handshake drops the connection). No
 * timeout fires during a session or in radio program mode. */
#include "app.h"

#include <string.h>

#include "acq.h"
#include "applog.h"
#include "link.h"
#include "buttons.h"
#include "charger.h"
#include "cw2015.h"
#include "diag.h"
#include "dsp_ppg_ctrl.h"
#include "hal.h"
#include "mcp4728.h"
#include "nav.h"
#include "radio.h"
#include "rails.h"
#include "settings_store.h"
#include "ssd1306.h"
#include "stream.h"
#include "stream_fmt.h"
#include "ui_model.h"

/* ---- policy constants ---------------------------------------------------- */
#define CONTACT_LOST_END_MS   2000u  /* PPG: finger off after RUNNING -> summary (ECG: acq.h) */
#define EVIDENCE_LOG_MS       1000u  /* one ecg / ppg line per second while armed */
#define NO_CONTACT_DISARM_MS  30000u /* WAIT_CONTACT -> back to the card */
#define MIN_VALID_MS          10000u /* summary needs >= 10 s ... */
#define MIN_VALID_BEATS       5u     /* ... and >= 5 beats / pulses */
#define BATT_POLL_MS          10000u
#define BATT_FAILS_INVALID    3u
#define BATT_RETRY_MS         100u   /* gauge not ready yet: read again soon */
#define BATT_SETTLE_MS        2000u  /* ... for this long after waking it */
#define BATT_CONFIRM_MS       1000u  /* a critical reading is re-read this much later */
#define BATT_CRIT_READS       2u     /* consecutive critical readings before shutdown */
#define STANDBY_BATT_WAKE_S   600u   /* timed wake in standby */
#define STATE_EVENT_MS        5000u
#define OVERLAY_USB_MS        2500u
#define OVERLAY_BATT_MS       3000u
#define OVERLAY_FAULT_MS      3000u
#define POWER_HOLD_SHOW_MS    300u
#define BOOT_SCREEN_MS        2000u  /* designer credit after reset / button wake */
#define POWER_DOWN_SCREEN_MS  1500u  /* ... and before STANDBY */
#define FRAME_FAST_MS         (1000u / UI_MAX_FPS)
#define FRAME_SLOW_MS         100u
#define SETTINGS_SAVE_IDLE_MS 3000u
#define DIAG_REFRESH_MS       250u
#define PPG_TIA_BIAS_CODE     304u   /* 0.245 V photodiode bias: PPG_HI near mid when dark */

typedef enum { SES_NONE, SES_USB_BLOCKED, SES_FAULT, SES_ACTIVE } ses_t;
typedef enum { THEN_NONE, THEN_SUMMARY_OR_CARD, THEN_LIVE, THEN_STANDBY } then_t;
typedef enum { END_USER, END_ABORT, END_LEAD, END_USB, END_BATT, END_TIMEOUT } end_t;

static struct {
    app_debug_t dbg;
    uint32_t now, prev_step_ms;
    uint32_t last_input_ms;
    bool power_off_pending;  /* BUTTON1 long: POWER_DOWN screen, then STANDBY */
    uint32_t splash_ms;      /* when UI_SCREEN_BOOT / POWER_DOWN was entered */

    /* navigation */
    ui_screen_t screen;
    uint8_t cursor;
    uint8_t diag_page;
    uint32_t diag_ms;

    /* overlays */
    ui_overlay_t overlay;
    uint32_t overlay_until;
    then_t then;
    bool overlay_modal; /* buttons ignored (critical battery) */
    const char *overlay_text;

    /* session */
    ses_t ses;
    ui_mode_t mode;
    uint32_t nocontact_ms;  /* since when there is no contact (with a heartbeat) */
    bool ever_running;
    bool start_logged;      /* VMID self-check result logged for this session */
    uint32_t acq_on_ms;     /* hal_millis() when the acquisition was first seen ON */
    uint32_t end_n, end_ms; /* ECG stream samples and ms since acq_on_ms at the session end */
    uint8_t log_state;      /* session state last logged (ecg / ppg token) */
    uint32_t log_ms;        /* last evidence line */
    const char *fault_text;
    ui_summary_t summary;
    bool have_session;
    acq_summary_t last;
    uint8_t log_gain_sw;    /* acq gain_switches already logged */

    /* battery */
    bool gauge_ok;
    uint32_t gauge_wake_ms;  /* cw2015_init() succeeded (gauge woken) */
    uint8_t batt_crit_n;     /* consecutive critical readings */
    uint8_t batt_fails;
    uint8_t soc_gauge;
    uint32_t batt_next_ms;
    uint32_t standby_rtc_due;

    /* settings */
    settings_t settings, saved;
    bool dirty;
    uint32_t dirty_ms;

    /* faults */
    bool mcp_ok;
    bool btn_fault_shown;

    /* display */
    uint32_t frame_ms;
    bool force_draw;
    bool blanked;
    uint32_t state_ev_ms;

    /* phone / radio */
    uint32_t last_phone_ms;  /* last LINK_CMD (STANDBY timeout input) */
    bool phone_dirty;        /* settings changed by the phone: deferred save */
    bool time_valid;
    uint32_t time_local;     /* local wall-clock seconds at time_rtc */
    uint32_t time_rtc;       /* hal_rtc_seconds() when set */
} S;

static ui_model_t M;
static uint8_t FB[OLED_FB_BYTES];
static char s_fault_buf[64];
static diag_info_t s_diag;
static char s_radio_fw[12], s_ble_id[5];

const app_debug_t *app_debug(void) { return &S.dbg; }
const ui_model_t *app_model(void) { return &M; }
const settings_t *app_settings(void) { return &S.settings; }

static void enter_standby(void);
static uint8_t start_session(ui_mode_t mode);

/* ======================================================================== */
/* helpers                                                                   */
/* ======================================================================== */

static bool usb_now(void) { return !hal_pin_read(PIN_CHG_PG); }
static bool usb_present(void) { return usb_now() || charger_usb_present(); }
static bool mode_has_ecg(ui_mode_t m) { return m != UI_MODE_PPG; }
static bool mode_has_ppg(ui_mode_t m) { return m != UI_MODE_ECG; }

static void redraw(void) { S.force_draw = true; }

/* ms -> whole seconds, saturating at the uint16 UI fields (18.2 h). */
static uint16_t secs_u16(uint32_t ms)
{
    uint32_t s = ms / 1000u;
    return (uint16_t)(s > UINT16_MAX ? UINT16_MAX : s);
}

static void show_overlay(ui_overlay_t ov, uint32_t ms, then_t then, const char *text, bool modal)
{
    S.overlay = ov;
    S.overlay_until = S.now + ms;
    S.then = then;
    S.overlay_text = text;
    S.overlay_modal = modal;
    redraw();
}

static void display_wake(void)
{
    if (S.dbg.pwr == APP_PWR_DISPLAY_OFF) {
        ssd1306_power_on();
        S.dbg.pwr = APP_PWR_AWAKE;
        S.blanked = false;
    }
    S.last_input_ms = S.now;
    redraw();
}

static void diag_exit(void)
{
    if (S.screen == UI_SCREEN_DIAG && S.diag_page == DIAG_PAGE_ANALOG)
        rails_off();
    if (S.screen == UI_SCREEN_DIAG && S.diag_page == DIAG_PAGE_RADIO)
        radio_program(false); /* normal power-down (no-op when not held) */
}

static void go_screen(ui_screen_t s)
{
    if (S.screen == UI_SCREEN_DIAG && s != UI_SCREEN_DIAG)
        diag_exit();
    S.screen = s;
    if (s != UI_SCREEN_SETTINGS)
        S.cursor = 0;
    redraw();
}

static bool splash_on(void)
{
    return S.screen == UI_SCREEN_BOOT || S.screen == UI_SCREEN_POWER_DOWN;
}

static void show_splash(ui_screen_t s)
{
    go_screen(s);
    S.splash_ms = S.now;
}

static uint8_t stream_mode(void)
{
    if (S.ses != SES_ACTIVE || acq_status()->phase != ACQ_ON)
        return 0;
    return S.mode == UI_MODE_ECG ? 1u : S.mode == UI_MODE_PPG ? 2u : 3u;
}

static void send_state(void)
{
    const acq_status_t *st = acq_status();
    uint8_t m = stream_mode();
    uint8_t flags = (uint8_t)((usb_present() ? 1u : 0u) |
                              (charger_state() == CHARGER_CHARGING ? 2u : 0u));
    stream_ev_state(m, m ? st->notch_hz : 0u, m ? st->mid_code : 0u,
                    S.dbg.batt_valid ? S.dbg.batt_pct : 0u, flags);
    S.state_ev_ms = S.now;
}

/* ======================================================================== */
/* acquisition sink                                                          */
/* ======================================================================== */

static void k_ecg_trace(const int16_t *uv, size_t n) { ui_trace_push(UI_TRACE_ECG, uv, (uint16_t)n); }
static void k_ppg_trace(const int16_t *v, size_t n) { ui_trace_push(UI_TRACE_PPG, v, (uint16_t)n); }
static void k_ecg_gap(size_t n) { ui_trace_gap(UI_TRACE_ECG, (uint16_t)n); }

/* The 30 s disarm counts from the arming or from the end of the last
 * contact; in ECG modes only a contact that delivered a beat restarts it
 * (open inputs that look stable for a while are not a person). */
static void k_contact(bool on)
{
    stream_ev_lead(on);
    if (!on && (!mode_has_ecg(S.mode) || acq_status()->contact_beat))
        S.nocontact_ms = hal_millis();
    redraw();
}

static void k_settled(bool on)
{
    if (on) {
        /* fresh sweep: the first one must not be scaled to the transient */
        ui_trace_reset(sweep_ms(S.settings.sweep));
        S.ever_running = true;
    }
    redraw();
}

static const acq_sink_t k_sink = {
    stream_ecg_block, stream_ppg_block, k_ecg_trace, k_ppg_trace,
    stream_ev_r_peak, stream_ev_pulse, k_contact, k_settled, k_ecg_gap,
};

/* ======================================================================== */
/* battery                                                                   */
/* ======================================================================== */

static bool reading_critical(uint16_t mv, uint8_t pct)
{
    return pct < BATT_CUTOFF_PCT || mv < BATT_CUTOFF_MV;
}

/* Gauge readings. Right after cw2015_init() woke the gauge, VCELL / SOC read
 * 0 until its first conversion (cw2015_read() -> HAL_ERR_PARAM): such a
 * reading is not used, it is retried every BATT_RETRY_MS (non-blocking) for
 * up to BATT_SETTLE_MS. After that a reading with VCELL != 0 is used (SOC >
 * 100 then falls back to the voltage estimate in batt_pct_combine()); one with
 * VCELL = 0 leaves the battery unknown. A critical reading is re-read after
 * BATT_CONFIRM_MS; the shutdown needs BATT_CRIT_READS in a row. */
static void battery_read(void)
{
    uint32_t now = hal_millis();
    if (!S.gauge_ok) {
        S.gauge_ok = cw2015_init() == HAL_OK;
        S.dbg.gauge_ok = S.gauge_ok;
        if (!S.gauge_ok) {
            S.dbg.batt_valid = false;
            return;
        }
        S.gauge_wake_ms = now;
    }
    uint16_t mv = 0;
    uint8_t soc = 0;
    hal_status_t st = cw2015_read(&mv, &soc);
    if (st == HAL_ERR_PARAM) {
        S.batt_fails = 0; /* the bus is fine */
        if (now - S.gauge_wake_ms < BATT_SETTLE_MS) {
            S.batt_next_ms = now + BATT_RETRY_MS; /* not ready yet */
            return;
        }
        if (mv == 0) {
            S.dbg.batt_valid = false;
            S.batt_crit_n = 0;
            return;
        }
        st = HAL_OK; /* VCELL valid, SOC implausible: voltage estimate */
    }
    if (st == HAL_OK) {
        S.batt_fails = 0;
        S.soc_gauge = soc;
        S.dbg.batt_mv = mv;
        S.dbg.batt_pct = batt_pct_combine(mv, soc);
        S.dbg.batt_valid = true;
        if (reading_critical(S.dbg.batt_mv, S.dbg.batt_pct)) {
            if (S.batt_crit_n < 255u)
                S.batt_crit_n++;
            if (S.batt_crit_n < BATT_CRIT_READS)
                S.batt_next_ms = now + BATT_CONFIRM_MS; /* confirm soon */
        } else {
            S.batt_crit_n = 0;
        }
    } else {
        S.dbg.i2c_errors++;
        if (++S.batt_fails >= BATT_FAILS_INVALID) {
            S.dbg.batt_valid = false;
            S.batt_crit_n = 0;
            S.gauge_ok = false;
            S.dbg.gauge_ok = false;
        }
    }
}

static bool batt_critical(void)
{
    return S.dbg.batt_valid && !usb_present() && S.batt_crit_n >= BATT_CRIT_READS &&
           reading_critical(S.dbg.batt_mv, S.dbg.batt_pct);
}

static bool batt_low(void)
{
    return S.dbg.batt_valid && !usb_present() && S.dbg.batt_pct < BATT_WARN_PCT;
}

/* ======================================================================== */
/* sessions                                                                  */
/* ======================================================================== */

static void make_summary(end_t why)
{
    const acq_summary_t *a = acq_summary();
    ui_summary_t *s = &S.summary;
    memset(s, 0, sizeof *s);
    s->mode = S.mode;
    s->duration_s = secs_u16(a->running_ms);
    if (S.mode == UI_MODE_PPG) {
        s->beats = a->pulses;
    } else {
        s->beats = a->hrv.beats;
        s->hr_avg = a->hrv.hr_avg;
        s->hr_min = a->hrv.hr_min;
        s->hr_max = a->hrv.hr_max;
        s->sdnn_ms = a->hrv.sdnn_ms;
        s->rmssd_ms = a->hrv.rmssd_ms;
    }
    if (mode_has_ppg(S.mode)) {
        s->pulse_avg = a->pulse_avg;
        s->pi_permille = a->pi_avg;
    }
    if (S.mode == UI_MODE_BOTH)
        s->pat_ms = a->pat_avg_ms;
    s->stopped_by_usb = why == END_USB;
    s->valid = a->running_ms >= MIN_VALID_MS && s->beats >= MIN_VALID_BEATS;
}

static void log_session_end(end_t why)
{
    static const char *const names[] = {"user", "abort", "lead-off", "usb", "battery", "timeout"};
    const acq_summary_t *a = acq_summary();
    log_begin("session");
    log_s(" end ");
    log_s(names[why]);
    log_s(" run_ms=");
    log_u(a->running_ms);
    log_s(" beats=");
    log_u(a->hrv.beats);
    log_s(" hr=");
    log_u(a->hrv.hr_avg);
    log_s(" pulses=");
    log_u(a->pulses);
    log_s(" pat=");
    log_u(a->pat_avg_ms);
    log_s(" lost=");
    log_u(a->ecg_lost + a->ppg_lost);
    log_s(" n=");
    log_u(S.end_n);
    log_s(" ms=");
    log_u(S.end_ms);
    if (a->ecg_resyncs || a->ppg_resyncs || hal_acq_resyncs()) {
        log_s(" resync=");
        log_u((uint32_t)a->ecg_resyncs + a->ppg_resyncs + hal_acq_resyncs());
    }
    log_end();
    if (mode_has_ecg(S.mode)) {
        /* bench A/B: the front end over the whole session, samples blanked
         * by FAST_RESTORE excluded */
        log_begin("session");
        log_s(" front-end rail=");
        log_u(a->ecg_raw_n ? (uint32_t)(((uint64_t)a->ecg_rail_n * 1000u + a->ecg_raw_n / 2u) /
                                        a->ecg_raw_n)
                           : 0u);
        log_s("/1000 of ");
        log_u(a->ecg_raw_n / ECG_RAW_HZ);
        log_s(" s, inr_max=");
        log_u(a->ecg_run_max_ms);
        log_s(" ms, fr=");
        log_u(a->fr_pulses);
        log_end();
    }
}

/* Every way out of a measurement comes through here (rails_off() inside
 * acq_stop()). Returns true if a summary should be shown. */
static bool end_session(end_t why)
{
    bool was_active = S.ses == SES_ACTIVE;
    /* timeline check for the log: ECG stream samples against the millisecond
     * clock (same oscillator), expected 1 sample per 2 ms */
    S.end_n = acq_status()->ecg_samples;
    S.end_ms = S.start_logged ? hal_millis() - S.acq_on_ms : 0u;
    acq_stop(); /* stop sequence; also a no-op safety net when idle */
    if (!was_active) {
        S.ses = SES_NONE;
        return false;
    }
    S.ses = SES_NONE;
    S.last = *acq_summary();
    S.have_session = true;
    make_summary(why);
    log_session_end(why);
    S.last_input_ms = S.now; /* the timeouts restart after a session */
    send_state();
    return S.ever_running;
}

static void to_summary_or_card(bool summary)
{
    go_screen(summary ? UI_SCREEN_SUMMARY : nav_card_of(S.mode));
}

/* ---- evidence log (RTT channel 0, text) ----------------------------------
 * State changes as they happen, and while armed one line per second per
 * signal (< 110 characters each, ~200 bytes/s in BOTH mode: the 1 KB
 * non-blocking RTT buffer drops whole lines when full):
 *   [t] ecg: <state> rail=<% of raw samples on a rail> min= max= avg=<raw
 *       codes> fr=<FAST_RESTORE pulses> beats=<so far> hr= mains=<uVpp>
 *       lost=<samples re-filled so far> rs=<resyncs>
 *   [t] ppg: <off|on|set|run> led=<LED DAC code> dac=<DAC12> dk= on=
 *       hi=<mean>/<min>-<max> (tia_dark, tia_on, hi_on means) c=<on - dark>
 *       agc=<LED steps> dcc=<DAC12 steps> p=<pulses> pr=<bpm> lost=<frames>
 * Values are of the last second unless "so far". */
static const char *const ecg_state_names[] = {"wait", "settle", "run", "hold", "end"};
static const char *const why_names[] = {"", "stable", "settled, beat", "rail", "no stable signal",
                                        "no beat"};

static uint8_t log_token(const acq_status_t *st)
{
    if (mode_has_ecg(S.mode))
        return st->ecg_state;
    return !st->contact ? 0u : st->settled ? 2u : 1u; /* wait / settle / run */
}

static void log_transition(const acq_status_t *st)
{
    if (mode_has_ecg(S.mode) && st->gain_switches != S.log_gain_sw) {
        S.log_gain_sw = st->gain_switches;
        log_begin("ecg");
        log_s(" auto-gain -> ");
        log_u(st->gain_hi ? (uint32_t)ECG_GAIN_HI : (uint32_t)ECG_GAIN_LO);
        log_end();
    }
    uint8_t t = log_token(st);
    if (t == S.log_state)
        return;
    bool ecg = mode_has_ecg(S.mode);
    const char *const *names = ecg_state_names;
    log_begin(ecg ? "ecg" : "ppg");
    log_s(" ");
    log_s(names[S.log_state]);
    log_s(" -> ");
    log_s(names[t]);
    if (ecg && st->ecg_why && st->ecg_why < sizeof why_names / sizeof why_names[0]) {
        log_s(" (");
        log_s(why_names[st->ecg_why]);
        log_s(")");
    }
    log_end();
    S.log_state = t;
}

static void log_mean(const char *k, uint32_t sum, uint32_t n)
{
    log_s(k);
    log_u(n ? (sum + n / 2u) / n : 0u);
}

static void log_evidence(const acq_status_t *st)
{
    if (S.now - S.log_ms < EVIDENCE_LOG_MS)
        return;
    S.log_ms = S.now;
    acq_diag_t d;
    acq_diag_take(&d);
    const acq_summary_t *a = acq_summary();
    if (mode_has_ecg(S.mode)) {
        log_begin("ecg");
        log_s(" ");
        log_s(ecg_state_names[st->ecg_state < 5u ? st->ecg_state : 0u]);
        if (d.ecg.n) {
            log_s(" rail=");
            log_u((d.ecg.rail * 100u + d.ecg.n / 2u) / d.ecg.n);
            log_s("% min=");
            log_u(d.ecg.min);
            log_s(" max=");
            log_u(d.ecg.max);
            log_mean(" avg=", d.ecg.sum, d.ecg.n);
            log_s(" inr=");
            log_u(d.ecg.run_max / (ECG_RAW_HZ / 1000u));
        } else {
            log_s(" no samples");
        }
        log_s(" fr=");
        log_u(d.fr_pulses);
        log_s(" beats=");
        log_u(st->ecg_beats);
        log_s(" hr=");
        log_u(st->hr_bpm);
        log_s(" mains=");
        log_u(st->mains_uvpp);
        log_s(" lost=");
        log_u(a->ecg_lost);
        log_s(" rs=");
        log_u((uint32_t)a->ecg_resyncs + hal_acq_resyncs());
        log_end();
    }
    if (mode_has_ppg(S.mode)) {
        log_begin("ppg");
        log_s(!st->finger ? " off" : S.mode == UI_MODE_BOTH ? " on" : st->settled ? " run" : " set");
        log_s(" led=");
        log_u(st->led_code);
        log_s(" dac=");
        log_u(st->dac12);
        if (d.ppg_n) {
            log_mean(" dk=", d.dark_sum, d.ppg_n);
            log_mean(" on=", d.on_sum, d.ppg_n);
            log_mean(" hi=", d.hi_sum, d.ppg_n);
            log_s("/");
            log_u(d.hi_min);
            log_s("-");
            log_u(d.hi_max);
            log_s(" c=");
            int32_t n = (int32_t)d.ppg_n;
            log_i(d.coarse_sum >= 0 ? (d.coarse_sum + n / 2) / n : -((n / 2 - d.coarse_sum) / n));
        } else {
            log_s(" no frames");
        }
        log_s(" agc=");
        log_u(st->agc_steps);
        log_s(" dcc=");
        log_u(st->dcc_steps);
        log_s(" p=");
        log_u(st->ppg_pulses);
        log_s(" pr=");
        log_u(st->pulse_bpm);
        log_s(" lost=");
        log_u(a->ppg_lost);
        log_end();
    }
}

/* Same rules for a button and a phone START; returns LINK_RES_*. */
static uint8_t start_session(ui_mode_t mode)
{
    S.mode = mode;
    S.ever_running = false;
    S.start_logged = false;
    S.log_state = 0;
    S.log_ms = S.now;
    S.fault_text = NULL;
    S.nocontact_ms = S.now;
    if (mode_has_ecg(mode) && usb_now()) {
        /* ECG only runs on battery: report, never raise MEASURE_REQ */
        S.ses = SES_USB_BLOCKED;
        go_screen(nav_live_of(mode));
        log_line("session", "blocked: USB power");
        return LINK_RES_REFUSED_USB;
    }
    if (batt_low()) {
        show_overlay(UI_OVERLAY_LOW_BATTERY, OVERLAY_BATT_MS, THEN_NONE, NULL, false);
        log_line("session", "refused: battery low");
        return LINK_RES_REFUSED_BATT;
    }
    if (mode_has_ppg(mode) && !S.mcp_ok) {
        S.mcp_ok = mcp4728_init() == HAL_OK; /* one more try */
        S.dbg.mcp_ok = S.mcp_ok;
        if (!S.mcp_ok) {
            S.ses = SES_FAULT;
            S.fault_text = "LED driver MCP4728 not answering: pulse disabled";
            go_screen(nav_live_of(mode));
            log_line("session", "refused: MCP4728 absent");
            return LINK_RES_FAULT;
        }
    }
    acq_cfg_t c;
    memset(&c, 0, sizeof c);
    c.ecg = mode_has_ecg(mode);
    c.ppg = mode_has_ppg(mode);
    c.gain_hi = S.settings.gain == GAIN_HI;
    c.gain_auto = S.settings.gain == GAIN_AUTO; /* starts low, switches in the session */
    c.rld = S.settings.rld;
    c.notch = S.settings.notch;
    c.led_limit_ma = led_limit_ma(S.settings.led_limit);
    c.restore = S.settings.restore;
    if (!acq_start(&c)) {
        S.ses = SES_FAULT;
        S.fault_text = "Could not start acquisition";
        go_screen(nav_live_of(mode));
        return LINK_RES_FAULT;
    }
    S.ses = SES_ACTIVE;
    S.dbg.sessions++;
    go_screen(nav_live_of(mode));
    log_begin("session");
    log_s(" start mode=");
    log_u((uint32_t)mode);
    log_s(" gain=");
    log_u(c.gain_hi ? (uint32_t)ECG_GAIN_HI : (uint32_t)ECG_GAIN_LO);
    if (c.gain_auto)
        log_s(" auto");
    S.log_gain_sw = 0;
    if (c.ecg) {
        static const char *const rld_names[] = {"both", "left", "right", "off"};
        static const char *const restore_names[] = {"fast", "slow", "off"};
        log_s(" rld=");
        log_s(c.rld < RLD_MODE_COUNT ? rld_names[c.rld] : "?");
        log_s(" restore=");
        log_s(c.restore < RESTORE_MODE_COUNT ? restore_names[c.restore] : "?");
    }
    log_end();
    return LINK_RES_OK;
}

static const char *fault_text_of(const acq_status_t *st)
{
    switch (st->err) {
    case ACQ_ERR_VMID: {
        char *p = s_fault_buf, *e = s_fault_buf + sizeof s_fault_buf - 1;
        p = fmt_s(p, e, "VMID ");
        p = fmt_u(p, e, st->vmid_mv);
        p = fmt_s(p, e, " mV, needs ");
        p = fmt_u(p, e, VDDA_MV / 2u);
        p = fmt_s(p, e, "+-");
        p = fmt_u(p, e, ECG_VMID_TOL_MV);
        p = fmt_s(p, e, ". Check +3V3A, U7B");
        return s_fault_buf;
    }
    case ACQ_ERR_LED_DAC: return "LED driver MCP4728 did not accept the current";
    default: return "Analog start-up failed";
    }
}

static void session_update(void)
{
    if (S.ses != SES_ACTIVE)
        return;
    const acq_status_t *st = acq_status();
    if (st->phase == ACQ_FAILED) {
        if (st->err == ACQ_ERR_USB) {
            S.ses = SES_USB_BLOCKED;
        } else {
            S.ses = SES_FAULT;
            S.fault_text = fault_text_of(st);
        }
        log_begin("session");
        log_s(" start failed err=");
        log_u((uint32_t)st->err);
        log_s(" vmid=");
        log_u(st->vmid_mv);
        log_end();
        acq_stop();
        redraw();
        return;
    }
    if (st->phase == ACQ_ON && !S.start_logged) {
        /* VMID self-check value on RTT at every start (ECG_VMID_TOL_MV is
         * checked in acq.c; a bring-up board can be judged from the log) */
        S.start_logged = true;
        S.acq_on_ms = hal_millis();
        if (mode_has_ecg(S.mode)) {
            log_begin("session");
            log_s(" vmid=");
            log_u(st->vmid_mv);
            log_s(" mV mid=");
            log_u(st->mid_code);
            log_s(" tol=+-");
            log_u(ECG_VMID_TOL_MV);
            log_end();
        }
    }
    if (st->phase != ACQ_ON)
        return;
    log_transition(st);
    if (mode_has_ecg(S.mode) && st->ecg_state == ACQ_ECG_END) {
        /* end of contact after RUNNING (acq.h decides from the signal) */
        to_summary_or_card(end_session(END_LEAD));
        return;
    }
    log_evidence(st);
    if (st->contact)
        return;
    uint32_t off = S.now - S.nocontact_ms;
    if (S.ever_running && !mode_has_ecg(S.mode)) {
        if (off > CONTACT_LOST_END_MS)
            to_summary_or_card(end_session(END_LEAD));
    } else if (!S.ever_running && off > NO_CONTACT_DISARM_MS) {
        end_session(END_TIMEOUT);
        go_screen(nav_card_of(S.mode));
    }
}

/* ======================================================================== */
/* overlays                                                                  */
/* ======================================================================== */

static void overlay_finish(void)
{
    then_t then = S.then;
    S.overlay = UI_OVERLAY_NONE;
    S.then = THEN_NONE;
    S.overlay_modal = false;
    redraw();
    switch (then) {
    case THEN_SUMMARY_OR_CARD: to_summary_or_card(S.ever_running); break;
    case THEN_LIVE: go_screen(nav_live_of(S.mode)); break;
    case THEN_STANDBY: enter_standby(); break;
    default: break;
    }
}

static void overlay_update(void)
{
    if (S.overlay != UI_OVERLAY_NONE && (int32_t)(S.now - S.overlay_until) >= 0)
        overlay_finish();
}

/* ======================================================================== */
/* battery policy                                                            */
/* ======================================================================== */

static void battery_policy(void)
{
    if (S.overlay == UI_OVERLAY_BATT_CRITICAL || S.screen == UI_SCREEN_POWER_DOWN)
        return; /* (powering down anyway) */
    if (batt_critical()) {
        log_begin("batt");
        log_s(" critical mv=");
        log_u(S.dbg.batt_mv);
        log_s(" pct=");
        log_u(S.dbg.batt_pct);
        log_end();
        end_session(END_BATT);
        if (S.dbg.pwr == APP_PWR_DISPLAY_OFF)
            display_wake();
        show_overlay(UI_OVERLAY_BATT_CRITICAL, OVERLAY_BATT_MS, THEN_STANDBY, NULL, true);
        return;
    }
    if (batt_low() && S.ses == SES_ACTIVE) {
        log_line("batt", "low: measurement stopped");
        bool sum = end_session(END_BATT);
        S.ever_running = sum;
        show_overlay(UI_OVERLAY_LOW_BATTERY, OVERLAY_BATT_MS, THEN_SUMMARY_OR_CARD, NULL, false);
    }
}

static void battery_poll(void)
{
    if ((int32_t)(S.now - S.batt_next_ms) < 0)
        return;
    S.batt_next_ms = S.now + BATT_POLL_MS;
    battery_read();
    battery_policy();
}

/* ======================================================================== */
/* USB / charger events                                                      */
/* ======================================================================== */

static void on_usb(uint32_t ev)
{
    if (ev & HAL_EV_USB_PLUGGED) {
        log_line("usb", "plugged");
        /* MEASURE_REQ is already low (CHG_PG ISR). End the ECG record now. */
        if (S.ses == SES_ACTIVE && mode_has_ecg(S.mode)) {
            bool sum = end_session(END_USB);
            S.ever_running = sum;
            if (!sum)
                S.ses = SES_USB_BLOCKED;
            display_wake();
            show_overlay(UI_OVERLAY_USB_STOPPED, OVERLAY_USB_MS,
                         sum ? THEN_SUMMARY_OR_CARD : THEN_LIVE, NULL, false);
        } else if (S.ses == SES_NONE || S.ses == SES_FAULT) {
            S.ses = SES_NONE;
            display_wake();
            if (S.overlay == UI_OVERLAY_NONE && !S.power_off_pending)
                go_screen(UI_SCREEN_HOME); /* also ends the boot screen */
        } else {
            display_wake(); /* PPG may run on USB */
        }
    }
    if (ev & HAL_EV_USB_REMOVED) {
        log_line("usb", "removed");
        if (S.ses == SES_USB_BLOCKED && !usb_now()) {
            display_wake();
            start_session(S.mode); /* the user is on the ECG screen: arm it */
        } else if (S.ses == SES_NONE) {
            display_wake();
            if (S.overlay == UI_OVERLAY_NONE && !S.power_off_pending)
                go_screen(UI_SCREEN_HOME);
        } else {
            display_wake();
        }
    }
}

/* ======================================================================== */
/* buttons / navigation                                                      */
/* ======================================================================== */

static void diag_enter(void)
{
    go_screen(UI_SCREEN_DIAG);
    S.diag_page = 0;
    S.diag_ms = 0;
}

static void diag_next(void)
{
    if (S.diag_page == DIAG_PAGE_ANALOG)
        rails_off();
    if (S.diag_page == DIAG_PAGE_RADIO)
        radio_program(false);
    S.diag_page = (uint8_t)((S.diag_page + 1u) % DIAG_PAGES);
    if (S.diag_page == DIAG_PAGE_ANALOG) {
        /* analog page: ANA_EN on (no MEASURE_REQ, no LEDs), TIA biased */
        rails_analog_on();
        rails_ppg_analog_on(dsp_ppg_dcc_code(PPG_TIA_BIAS_CODE));
    }
    S.diag_ms = 0;
    redraw();
}

static void do_action(const nav_result_t *r)
{
    switch (r->act) {
    case NAV_GOTO:
        if (S.screen == UI_SCREEN_SETTINGS && S.dirty)
            S.dirty_ms = S.now - SETTINGS_SAVE_IDLE_MS; /* save soon */
        if (S.ses != SES_ACTIVE && S.ses != SES_NONE)
            S.ses = SES_NONE;
        go_screen(r->screen);
        break;
    case NAV_START:
        (void)start_session(r->mode);
        break;
    case NAV_SET_CHANGE:
        settings_cycle(&S.settings, S.cursor);
        if (S.cursor == SET_ITEM_BLE && S.settings.ble == BLE_ON)
            radio_rearm(); /* "BLE turned on": a new advertising window */
        S.dirty = true;
        S.dirty_ms = S.now;
        redraw();
        break;
    case NAV_SET_NEXT:
        S.cursor++;
        redraw();
        break;
    case NAV_MEAS_END:
        if (S.ses == SES_ACTIVE)
            to_summary_or_card(end_session(END_USER));
        else {
            S.ses = SES_NONE;
            go_screen(nav_card_of(S.mode));
        }
        break;
    case NAV_HOME_ABORT:
        end_session(END_ABORT);
        S.ses = SES_NONE;
        go_screen(UI_SCREEN_HOME);
        break;
    case NAV_POWER_OFF:
        end_session(END_ABORT);
        diag_exit();
        S.power_off_pending = true; /* standby once the button is released */
        log_line("power", "off requested");
        redraw();
        break;
    case NAV_DIAG_ENTER: diag_enter(); break;
    case NAV_DIAG_NEXT: diag_next(); break;
    case NAV_DIAG_LEAVE:
        if (S.diag_page == DIAG_PAGE_RADIO) {
            /* RADIO page: BUTTON1 toggles program mode instead of leaving */
            radio_program(!radio_program_mode());
            S.diag_ms = 0;
            redraw();
        } else {
            go_screen(UI_SCREEN_ABOUT);
        }
        break;
    default: break;
    }
}

/* Boot screen: Home after BOOT_SCREEN_MS, or at once on a new press, which
 * then does nothing else (ignored until released). The press that woke the
 * card, or one held since reset, is already ignored: no hold time. */
static void boot_screen_update(void)
{
    if (S.screen != UI_SCREEN_BOOT)
        return;
    bool press = buttons_hold_ms(BTN_1) != 0 || buttons_hold_ms(BTN_2) != 0;
    if (press)
        buttons_ignore_until_release();
    if (press || S.now - S.splash_ms >= BOOT_SCREEN_MS) {
        S.last_input_ms = S.now; /* the display timeout counts from Home */
        go_screen(UI_SCREEN_HOME);
    }
}

static void handle_buttons(void)
{
    btn_event_t e;
    while ((e = buttons_get_event()) != BTN_EV_NONE) {
        S.last_input_ms = S.now;
        radio_user(); /* a user wake re-arms the radio after an advertising timeout */
        if (e == BTN_EV_BTN1_SHORT || e == BTN_EV_BTN2_SHORT || e == BTN_EV_BTN1_LONG ||
            e == BTN_EV_BTN2_LONG)
            stream_ev_button(e == BTN_EV_BTN1_SHORT || e == BTN_EV_BTN1_LONG ? 1u : 2u,
                             e == BTN_EV_BTN1_LONG || e == BTN_EV_BTN2_LONG ? 1u : 0u);
        if (S.power_off_pending || S.dbg.pwr != APP_PWR_AWAKE || splash_on())
            continue;
        if (S.overlay != UI_OVERLAY_NONE) {
            if (S.overlay_modal)
                continue;
            if (e != BTN_EV_BTN1_LONG) { /* any press dismisses */
                overlay_finish();
                continue;
            }
            S.overlay = UI_OVERLAY_NONE;
        }
        nav_result_t r = nav_on_button(S.screen, S.cursor, S.mode, e);
        do_action(&r);
    }
    if (buttons_any_down())
        S.last_input_ms = S.now;
    if (buttons_fault() && !S.btn_fault_shown && !splash_on()) { /* after the boot screen */
        S.btn_fault_shown = true;
        S.overlay_text = !hal_pin_read(PIN_BUTTON1) ? "Button 1 stuck down: it is ignored"
                                                    : "Button 2 stuck down: it is ignored";
        log_line("fault", S.overlay_text);
        show_overlay(UI_OVERLAY_FAULT, OVERLAY_FAULT_MS, THEN_NONE, S.overlay_text, false);
    }
}

/* ======================================================================== */
/* power policy                                                              */
/* ======================================================================== */

static void display_off(void)
{
    if (S.screen == UI_SCREEN_DIAG) {
        diag_exit();
        S.screen = UI_SCREEN_ABOUT;
    }
    ssd1306_power_off(); /* ~100 ms; only with acquisition stopped */
    S.dbg.pwr = APP_PWR_DISPLAY_OFF;
    log_line("power", "display off");
}

static void power_policy(void)
{
    if (S.power_off_pending) {
        if (S.screen != UI_SCREEN_POWER_DOWN) {
            /* the long press is released: the power-down screen */
            if (!buttons_any_down()) {
                rails_off(); /* already off (end_session); never on past here */
                S.overlay = UI_OVERLAY_NONE;
                S.then = THEN_NONE;
                show_splash(UI_SCREEN_POWER_DOWN);
                log_line("power", "power-down screen");
            }
        } else if (S.now - S.splash_ms >= POWER_DOWN_SCREEN_MS && !buttons_any_down()) {
            enter_standby(); /* a press during the screen: standby at its release */
        }
        return;
    }
    if (acq_active() || S.overlay != UI_OVERLAY_NONE || radio_program_mode()) {
        /* armed with contact (or about to be), or the radio held for
         * programming: no timeouts */
        S.last_input_ms = S.now;
        return;
    }
    uint32_t idle = S.now - S.last_input_ms;
    /* phone commands keep the card out of STANDBY (not the display on) */
    uint32_t idle_sb = idle;
    if (S.now - S.last_phone_ms < idle_sb)
        idle_sb = S.now - S.last_phone_ms;
    if (S.dbg.pwr == APP_PWR_AWAKE && idle >= UI_DISPLAY_TIMEOUT_S * 1000u)
        display_off();
    else if (S.dbg.pwr == APP_PWR_DISPLAY_OFF && idle_sb >= UI_STANDBY_TIMEOUT_S * 1000u)
        enter_standby();
}

static void save_settings_if_needed(bool force)
{
    if (!S.dirty || acq_active() || rails_analog_is_on())
        return;
    /* on the Settings page, and after a phone SET (a client may send several
     * in a row, and a flash write masks the UART RX interrupt), wait until
     * the changes stop */
    if (!force && (S.screen == UI_SCREEN_SETTINGS || S.phone_dirty) &&
        S.now - S.dirty_ms < SETTINGS_SAVE_IDLE_MS)
        return;
    S.dirty = false;
    S.phone_dirty = false;
    if (memcmp(&S.settings, &S.saved, sizeof S.settings) == 0)
        return; /* cycled back to the stored values */
    if (settings_save(&S.settings) == HAL_OK) {
        S.saved = S.settings;
        S.dbg.settings_writes++;
        log_line("settings", "saved");
    } else {
        log_line("settings", "save FAILED");
    }
}

static void enter_standby(void)
{
    end_session(END_ABORT);
    diag_exit();
    rails_off();
    radio_off_now(); /* shutdown handshake: radio unpowered, pins safe */
    save_settings_if_needed(true);
    if (ssd1306_is_powered())
        ssd1306_power_off();
    S.power_off_pending = false;
    S.overlay = UI_OVERLAY_NONE;
    S.then = THEN_NONE;
    S.dbg.pwr = APP_PWR_STANDBY;
    S.dbg.standby_entries++;
    S.standby_rtc_due = hal_rtc_seconds() + STANDBY_BATT_WAKE_S;
    log_line("power", "standby");
    (void)hal_events_take(); /* drop the edges of the power-off press */
}

static void wake_up(uint32_t cause)
{
    S.now = hal_millis();
    S.dbg.wakes++;
    S.dbg.pwr = APP_PWR_AWAKE;
    charger_init(); /* adopt the pins as they are now */
    charger_tick(S.now);
    if (cause & HAL_EV_BUTTON)
        buttons_ignore_until_release(); /* the wake press does nothing */
    radio_rearm(); /* a fresh advertising window (and a retry after a fault) */
    ssd1306_power_on();
    S.blanked = false;
    /* a button wake is the user's "power on": the boot screen; USB goes
     * straight to Home (on_usb()) */
    bool usb = (cause & (HAL_EV_USB_PLUGGED | HAL_EV_USB_REMOVED)) != 0;
    S.screen = (cause & HAL_EV_BUTTON) && !usb ? UI_SCREEN_BOOT : UI_SCREEN_HOME;
    S.splash_ms = S.now;
    S.cursor = 0;
    S.overlay = UI_OVERLAY_NONE;
    S.last_input_ms = S.now;
    S.prev_step_ms = S.now;
    S.batt_next_ms = S.now + BATT_POLL_MS;
    battery_read();
    battery_policy();
    redraw();
    log_line("power", "wake");
    send_state();
}

#define WAKE_EVENTS (HAL_EV_BUTTON | HAL_EV_USB_PLUGGED | HAL_EV_USB_REMOVED)

static void standby_step(void)
{
    uint32_t rtc = hal_rtc_seconds();
    uint32_t rem = (int32_t)(S.standby_rtc_due - rtc) > 0 ? S.standby_rtc_due - rtc : 1u;
    uint32_t cause = hal_standby(rem);
    hal_wdt_kick();
    if (cause & WAKE_EVENTS) {
        wake_up(cause); /* the bits stay latched: app_step() handles them */
        return;
    }
    if (cause & HAL_EV_CW_ALRT)
        (void)cw2015_clear_alert();
    if ((int32_t)(hal_rtc_seconds() - S.standby_rtc_due) >= 0) {
        /* periodic battery check only: display stays off, back to sleep */
        S.dbg.timed_wakes++;
        S.standby_rtc_due = hal_rtc_seconds() + STANDBY_BATT_WAKE_S;
        battery_read();
    }
    /* Take every bit still latched. hal_standby() returns at once while any
     * wake-capable bit is pending, so one nobody handles here (RADIO_IRQ in
     * this build, a CW_ALRT that came back) would otherwise make the loop
     * spin. A button / USB edge that arrived meanwhile is a real wake. */
    uint32_t rest = hal_events_take();
    if (rest & WAKE_EVENTS) {
        wake_up(rest);
        if (rest & (HAL_EV_USB_PLUGGED | HAL_EV_USB_REMOVED))
            on_usb(rest); /* what app_step() would have done with the bits */
        return;
    }
    if (rest & HAL_EV_CW_ALRT)
        (void)cw2015_clear_alert();
}


/* ======================================================================== */
/* radio link: phone commands, LINK_STATUS, policy                           */
/* ======================================================================== */

static ui_meas_state_t live_state(const acq_status_t *st);

static void wall_clock(uint8_t *hour, uint8_t *minute)
{
    uint32_t t = S.time_local + (hal_rtc_seconds() - S.time_rtc);
    *hour = (uint8_t)((t / 3600u) % 24u);
    *minute = (uint8_t)((t / 60u) % 60u);
}

static uint8_t phone_start(ui_mode_t mode)
{
    if (radio_program_mode() || S.power_off_pending || S.screen == UI_SCREEN_DIAG)
        return LINK_RES_BUSY;
    if (S.overlay == UI_OVERLAY_BATT_CRITICAL || batt_critical())
        return LINK_RES_REFUSED_BATT;
    if (S.overlay != UI_OVERLAY_NONE || S.ses == SES_ACTIVE)
        return LINK_RES_BUSY;
    display_wake(); /* the screen follows the phone */
    S.ses = SES_NONE; /* a USB / fault screen still showing */
    return start_session(mode);
}

static uint8_t phone_stop(void)
{
    if (radio_program_mode())
        return LINK_RES_BUSY;
    if (S.ses == SES_ACTIVE) {
        display_wake();
        to_summary_or_card(end_session(END_USER));
    } else if (S.ses != SES_NONE) {
        display_wake();
        S.ses = SES_NONE;
        go_screen(nav_card_of(S.mode));
    }
    return LINK_RES_OK;
}

static uint8_t phone_set(uint8_t item, uint8_t value)
{
    if (radio_program_mode())
        return LINK_RES_BUSY;
    bool measuring_item = item == SET_ITEM_GAIN || item == SET_ITEM_NOTCH || item == SET_ITEM_RLD ||
                          item == SET_ITEM_LED_LIMIT || item == SET_ITEM_RESTORE;
    if (measuring_item && S.ses == SES_ACTIVE)
        return LINK_RES_BUSY; /* applied at the start of a session */
    settings_t n = S.settings;
    if (!settings_set(&n, item, value))
        return LINK_RES_BAD_ARG;
    if (memcmp(&n, &S.settings, sizeof n) != 0) {
        if (item == SET_ITEM_BLE && value == BLE_ON)
            radio_rearm();
        S.settings = n;
        S.dirty = true;
        S.phone_dirty = true;
        S.dirty_ms = S.now;
        redraw();
    }
    return LINK_RES_OK;
}

static void phone_set_time(uint32_t unix_s, int16_t offset_min)
{
    S.time_local = (uint32_t)((int32_t)unix_s + (int32_t)offset_min * 60);
    S.time_rtc = hal_rtc_seconds();
    S.time_valid = true;
    uint8_t h, m;
    wall_clock(&h, &m);
    log_begin("time");
    log_s(" set ");
    log_u(h);
    log_s(":");
    log_u(m / 10u);
    log_u(m % 10u);
    log_s(" (UTC");
    log_s(offset_min < 0 ? "-" : "+");
    log_u((uint32_t)(offset_min < 0 ? -offset_min : offset_min));
    log_s(" min)");
    log_end();
    redraw();
}

static uint8_t app_command(const link_cmd_t *c)
{
    S.last_phone_ms = S.now;
    S.dbg.phone_cmds++;
    switch (c->cmd) {
    case LINK_CMD_START:
        return phone_start(c->mode == 1u ? UI_MODE_ECG : c->mode == 2u ? UI_MODE_PPG : UI_MODE_BOTH);
    case LINK_CMD_STOP: return phone_stop();
    case LINK_CMD_SET: return phone_set(c->item, c->value);
    case LINK_CMD_SET_TIME: phone_set_time(c->unix_s, c->utc_offset_min); return LINK_RES_OK;
    case LINK_CMD_GET_STATUS: return LINK_RES_OK; /* radio.c sends LINK_STATUS */
    default: return LINK_RES_UNKNOWN;
    }
}

static void app_status(uint8_t o[LINK_STATUS_LEN])
{
    const acq_status_t *st = acq_status();
    bool ses = S.ses != SES_NONE, act = S.ses == SES_ACTIVE;
    bool on = act && st->phase == ACQ_ON;
    memset(o, 0, LINK_STATUS_LEN);
    o[0] = LINK_PROTO_VERSION;
    o[1] = ses ? (uint8_t)(S.mode == UI_MODE_ECG ? 1u : S.mode == UI_MODE_PPG ? 2u : 3u) : 0u;
    o[2] = ses ? (uint8_t)live_state(st) : 0xFFu;
    o[3] = S.dbg.batt_valid ? S.dbg.batt_pct : 255u;
    uint8_t f = 0;
    if (usb_present()) f |= LINK_ST_USB;
    if (charger_state() == CHARGER_CHARGING) f |= LINK_ST_CHARGING;
    if (charger_state() == CHARGER_FAULT) f |= LINK_ST_CHARGE_FAULT;
    if (on && !st->contact) f |= LINK_ST_NO_CONTACT;
    if (batt_low()) f |= LINK_ST_BATT_LOW;
    if (on && st->gain_hi) f |= LINK_ST_GAIN_HI;
    if (S.time_valid) f |= LINK_ST_TIME_VALID;
    o[4] = f;
    o[5] = S.settings.gain;
    o[6] = S.settings.notch;
    o[7] = S.settings.rld;
    o[8] = S.settings.led_limit;
    o[9] = S.settings.sweep;
    o[10] = on && mode_has_ecg(S.mode) ? st->notch_hz : 0u;
    uint16_t hr = !act ? 0u : S.mode == UI_MODE_PPG ? st->pulse_bpm : st->hr_bpm;
    o[11] = (uint8_t)(hr > 255u ? 255u : hr);
    uint16_t mv = S.dbg.batt_valid ? S.dbg.batt_mv : 0u;
    o[12] = (uint8_t)mv;
    o[13] = (uint8_t)(mv >> 8);
    o[14] = APP_FW_MAJOR;
    o[15] = APP_FW_MINOR;
    o[16] = APP_FW_PATCH;
    uint32_t up = S.now / 60000u;
    if (up > 0xFFFFu)
        up = 0xFFFFu;
    o[18] = (uint8_t)up;
    o[19] = (uint8_t)(up >> 8);
}

static const radio_ops_t k_radio_ops = {app_command, app_status};

/* Bluetooth On, card awake, battery not low (program mode overrides in radio.c). */
static void radio_step(void)
{
    bool want = S.settings.ble == BLE_ON && S.dbg.pwr != APP_PWR_STANDBY && !batt_low() &&
                !batt_critical() && S.overlay != UI_OVERLAY_BATT_CRITICAL;
    radio_want(want);
    radio_poll(S.now);
}

static ui_ble_t ble_ui_state(void)
{
    switch (radio_state()) {
    case RADIO_POWERING:
    case RADIO_RETRY_GAP:
    case RADIO_ADVERTISING: return UI_BLE_ADVERTISING;
    case RADIO_CONNECTED: return UI_BLE_CONNECTED;
    case RADIO_FAULT: return S.settings.ble == BLE_ON ? UI_BLE_FAULT : UI_BLE_OFF;
    default: return UI_BLE_OFF;
    }
}

/* ======================================================================== */
/* display                                                                   */
/* ======================================================================== */

static ui_meas_state_t live_state(const acq_status_t *st)
{
    switch (S.ses) {
    case SES_USB_BLOCKED: return UI_MEAS_USB_BLOCKED;
    case SES_FAULT: return UI_MEAS_FAULT;
    case SES_ACTIVE:
        if (st->phase != ACQ_ON || !st->contact)
            return UI_MEAS_WAIT_CONTACT;
        return st->settled ? UI_MEAS_RUNNING : UI_MEAS_SETTLING;
    default: return UI_MEAS_WAIT_CONTACT;
    }
}

static void build_model(void)
{
    const acq_status_t *st = acq_status();
    memset(&M, 0, sizeof M);
    M.screen = S.screen;
    M.now_ms = S.now;
    if (splash_on()) {
        uint32_t t = S.now - S.splash_ms;
        M.screen_ms = (uint16_t)(t > UINT16_MAX ? UINT16_MAX : t);
    }
    M.batt_pct = S.dbg.batt_pct;
    M.batt_valid = S.dbg.batt_valid;
    M.batt_mv = S.dbg.batt_valid ? S.dbg.batt_mv : 0u;
    M.usb_present = usb_present();
    M.charging = charger_state() == CHARGER_CHARGING;
    M.charge_fault = charger_state() == CHARGER_FAULT;
    M.settings = S.settings;
    M.settings_cursor = S.cursor;
    M.fw_version = APP_FW_VERSION;
    M.fault_text = S.fault_text;
    M.ble_state = ble_ui_state();
    const radio_info_t *ri = radio_info();
    if (ri->hello) {
        memcpy(s_radio_fw, ri->fw_str, sizeof s_radio_fw);
        memcpy(s_ble_id, ri->id_str, sizeof s_ble_id);
        M.radio_fw = s_radio_fw;
        M.ble_id = s_ble_id;
    }
    M.time_valid = S.time_valid;
    if (S.time_valid)
        wall_clock(&M.hour, &M.minute);

    ui_live_t *l = &M.live;
    l->state = live_state(st);
    if (S.ses == SES_ACTIVE) {
        l->hr_bpm = st->hr_bpm;
        l->pulse_bpm = st->pulse_bpm;
        l->pat_ms = st->pat_ms;
        l->pi_permille = st->pi_permille;
        l->quality = st->quality;
        l->beat_seq = st->beat_seq;
        l->gain_hi = st->gain_hi;
        l->notch_hz = st->notch_hz;
        l->elapsed_s = secs_u16(st->running_ms);
        l->settle_pct = st->settle_pct;
        /* ECG trace scale: the median QRS (0 until the first beats) */
        ui_trace_ecg_ref(mode_has_ecg(S.mode) ? st->qrs_uv : 0u, st->qrs_neg);
    }
    M.summary = S.summary;

    M.diag_page = S.diag_page;
    M.diag_pages = DIAG_PAGES;
    if (S.screen == UI_SCREEN_DIAG)
        diag_format(S.diag_page, &s_diag, M.diag);

    /* overlays: critical > power-off hold > timed */
    uint32_t hold = buttons_hold_ms(BTN_1);
    bool combo = buttons_hold_ms(BTN_2) != 0;
    if (S.overlay == UI_OVERLAY_BATT_CRITICAL) {
        M.overlay = S.overlay;
    } else if (S.power_off_pending) {
        if (S.screen != UI_SCREEN_POWER_DOWN) { /* "Release the button" */
            M.overlay = UI_OVERLAY_POWER_OFF;
            M.hold_progress = 100;
        }
    } else if (hold >= POWER_HOLD_SHOW_MS && !combo) {
        M.overlay = UI_OVERLAY_POWER_OFF;
        uint32_t p = hold * 100u / BTN_LONG_MS;
        M.hold_progress = (uint8_t)(p > 100u ? 100u : p);
    } else if (S.overlay != UI_OVERLAY_NONE) {
        M.overlay = S.overlay;
        if (S.overlay == UI_OVERLAY_FAULT)
            M.fault_text = S.overlay_text;
    }
}

static void diag_update(void)
{
    if (S.screen != UI_SCREEN_DIAG || S.now - S.diag_ms < DIAG_REFRESH_MS)
        return;
    S.diag_ms = S.now;
    diag_info_t *d = &s_diag;
    d->chg_pg = hal_pin_read(PIN_CHG_PG);
    d->chg_stat1 = hal_pin_read(PIN_CHG_STAT1);
    d->btn1_raw = !hal_pin_read(PIN_BUTTON1);
    d->btn2_raw = !hal_pin_read(PIN_BUTTON2);
    d->btn_fault = buttons_fault();
    d->charger = charger_state_name(charger_state());
    d->reset_cause = (uint8_t)hal_reset_cause();
    d->uptime_s = S.now / 1000u;
    d->analog_on = rails_analog_is_on();
    d->analog_ms = S.now - rails_analog_on_ms();
    if (S.diag_page == DIAG_PAGE_ANALOG && d->analog_on && !hal_acq_running()) {
        d->vmid = hal_adc_read_once(HAL_ADC_VMID);
        d->ecg = hal_adc_read_once(HAL_ADC_ECG);
        d->tia = hal_adc_read_once(HAL_ADC_TIA);
        d->ppg_hi = hal_adc_read_once(HAL_ADC_PPG_HI);
    }
    if (S.diag_page == 2) {
        d->ack60 = hal_i2c_xfer(I2C_ADDR_MCP4728, NULL, 0, NULL, 0) == HAL_OK;
        d->ack62 = hal_i2c_xfer(I2C_ADDR_CW2015, NULL, 0, NULL, 0) == HAL_OK;
        if (!S.gauge_ok)
            battery_read();
    }
    d->gauge_ok = S.gauge_ok;
    d->cw_version = cw2015_version();
    d->vcell_mv = S.dbg.batt_mv;
    d->soc_gauge = S.soc_gauge;
    d->batt_pct = S.dbg.batt_pct;
    d->batt_valid = S.dbg.batt_valid;
    d->mcp_ok = S.mcp_ok;
    d->mcp_eeprom_written = mcp4728_eeprom_was_written();
    d->i2c_errors = S.dbg.i2c_errors + rails_i2c_errors();
    d->have_session = S.have_session;
    d->ecg_lost = S.last.ecg_lost;
    d->ppg_lost = S.last.ppg_lost;
    d->stream_drops = stream_drops();
    d->stream_frames = stream_frames();
    d->loop_max_ms = S.dbg.loop_max_ms;
    d->loop_acq_max_ms = S.dbg.loop_acq_max_ms;
    d->led_code = S.last.led_code;
    d->dac12 = S.last.dac12;
    d->mid_code = S.last.mid_code;
    d->gain_hi = S.last.gain_hi;
    d->notch_hz = S.last.notch_hz;
    d->acq_i2c_errors = S.last.i2c_errors;
    d->fw_version = APP_FW_VERSION;
    d->settings_status = S.dbg.settings_status;
    d->settings_writes = S.dbg.settings_writes;
    d->radio = radio_info();
    d->radio_program = radio_program_mode();
    d->radio_tx_frames = link_tx_frames();
    d->radio_tx_drops = link_tx_drops();
    hal_uart_stats(&d->uart);
    redraw();
}

#ifdef APP_PPG_BLANK_DISPLAY
static bool live_running(void)
{
    return S.ses == SES_ACTIVE && nav_is_live(S.screen) && acq_status()->settled;
}
#endif

static void display_update(void)
{
    if (S.dbg.pwr != APP_PWR_AWAKE || !ssd1306_is_powered())
        return;
    (void)ssd1306_flush_poll();
#ifdef APP_PPG_BLANK_DISPLAY
    /* Open question 10: blank the panel (AEh) during PPG captures. */
    bool blank = live_running() && mode_has_ppg(S.mode);
    if (blank != S.blanked && !ssd1306_busy() && ssd1306_display_on(!blank))
        S.blanked = blank;
    if (S.blanked)
        return;
#endif
    bool fast = nav_is_live(S.screen) || S.overlay != UI_OVERLAY_NONE || S.power_off_pending ||
                splash_on() || buttons_hold_ms(BTN_1) != 0;
    uint32_t period = fast ? FRAME_FAST_MS : FRAME_SLOW_MS;
    if (!S.force_draw && S.now - S.frame_ms < period)
        return;
    if (ssd1306_busy())
        return; /* the DMA still reads FB: never touch it */
    build_model();
    ui_render(&M, FB);
    if (ssd1306_flush_start(FB)) {
        S.frame_ms = S.now;
        S.force_draw = false;
        S.dbg.frames_drawn++;
    }
}

/* ======================================================================== */
/* init / step                                                               */
/* ======================================================================== */

void app_init(void)
{
    hal_init();
    rails_init(); /* every enable low, module state in sync */
    memset(&S, 0, sizeof S);
    S.now = hal_millis();

    /* FIRST I2C job: the MCP4728's factory EEPROM would drive ~12 mA into
     * the LEDs; this powers every channel down (and fixes the EEPROM). */
    hal_status_t mst = mcp4728_init();
    S.mcp_ok = mst == HAL_OK;
    S.dbg.mcp_ok = S.mcp_ok;

    log_begin("boot");
    log_s(" ECG Card " APP_FW_VERSION " reset=");
    log_u((uint32_t)hal_reset_cause());
    log_s(S.mcp_ok ? " mcp=ok" : " mcp=FAIL");
    if (mcp4728_eeprom_was_written())
        log_s(" (eeprom programmed)");
    log_end();

    S.dbg.settings_status = (uint8_t)settings_load(&S.settings);
    S.saved = S.settings;
    if (S.dbg.settings_status != SETTINGS_LOADED)
        log_line("settings", S.dbg.settings_status == SETTINGS_BLANK ? "blank: defaults"
                                                                      : "corrupt: defaults");

    S.gauge_ok = cw2015_init() == HAL_OK;
    S.gauge_wake_ms = hal_millis();
    S.dbg.gauge_ok = S.gauge_ok;
    if (!S.gauge_ok)
        log_line("batt", "CW2015 not answering: battery unknown");
    S.batt_next_ms = S.now + BATT_POLL_MS;
    battery_read(); /* may schedule a retry / confirmation sooner */

    charger_init();
    charger_tick(S.now);
    buttons_init(S.now);
    stream_init();
    radio_init(&k_radio_ops);
    stream_set_tap(radio_tap);
    acq_init(&k_sink);
    ui_init();
    ssd1306_power_on();
    S.now = hal_millis();
    S.screen = UI_SCREEN_BOOT; /* Home after BOOT_SCREEN_MS */
    S.splash_ms = S.now;
    S.last_input_ms = S.now;
    S.prev_step_ms = S.now;
    S.dbg.pwr = APP_PWR_AWAKE;
    redraw();
    send_state();
    (void)hal_events_take(); /* boot edges: the drivers read the pins */
    battery_policy();
}

void app_step(void)
{
    if (S.dbg.pwr == APP_PWR_STANDBY) {
        standby_step();
        return;
    }
    S.now = hal_millis();
    uint32_t gap = S.now - S.prev_step_ms;
    S.prev_step_ms = S.now;
    if (S.dbg.loops) {
        if (gap > S.dbg.loop_max_ms)
            S.dbg.loop_max_ms = gap;
        if (acq_active() && gap > S.dbg.loop_acq_max_ms)
            S.dbg.loop_acq_max_ms = gap;
    }
    S.dbg.loops++;

    uint32_t ev = hal_events_take();
    if (ev & (HAL_EV_USB_PLUGGED | HAL_EV_USB_REMOVED))
        on_usb(ev); /* first: an ECG record ends before anything else */
    if ((ev & HAL_EV_BUTTON) && S.dbg.pwr == APP_PWR_DISPLAY_OFF) {
        /* any button wakes the display without acting */
        buttons_ignore_until_release();
        display_wake();
        radio_user(); /* a user wake: the radio comes back after an advertising timeout */
    }

    acq_poll(S.now);

    buttons_tick(S.now);
    boot_screen_update(); /* before the events: a skipping press is swallowed */
    if (charger_tick(S.now))
        redraw();
    if (ev & HAL_EV_CW_ALRT) {
        (void)cw2015_clear_alert();
        S.batt_next_ms = S.now; /* read now */
    }
    handle_buttons();
    session_update();
    overlay_update();
    if (S.dbg.pwr == APP_PWR_STANDBY)
        return;
    battery_poll();
    if (S.dbg.pwr == APP_PWR_STANDBY)
        return;
    radio_step(); /* after the battery: commands and the policy see this pass's reading */
    power_policy();
    if (S.dbg.pwr == APP_PWR_STANDBY)
        return;
    save_settings_if_needed(false);
    diag_update();
    display_update();
    if (S.now - S.state_ev_ms >= STATE_EVENT_MS)
        send_state();

    hal_wdt_kick(); /* main loop only */
    hal_idle();
}

void app_main(void)
{
    app_init();
    for (;;)
        app_step();
}
