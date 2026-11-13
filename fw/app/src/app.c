/* Main loop and application state machine.
 *
 * One pass (app_step):
 *   1. events: USB plug first (end an ECG record at once), wake handling
 *   2. acq_poll()            drain the ECG / PPG rings, DSP, control loops
 *   3. buttons, charger      debounce, events -> navigation actions
 *   4. session supervisor    contact / settling / timeouts / summary
 *   5. battery, power policy, settings save, diagnostics
 *   6. display               render + DMA flush at <= 25 fps, never while busy
 *   7. watchdog kick, hal_idle()
 * In the off state a pass is one hal_standby() plus its wake handling.
 *
 * Nothing here blocks for more than a few ms while acquisition runs: the
 * display power-off (100 ms) and flash writes happen only with acquisition
 * stopped. */
#include "app.h"

#include <string.h>

#include "acq.h"
#include "applog.h"
#include "buttons.h"
#include "charger.h"
#include "cw2015.h"
#include "diag.h"
#include "dsp_ppg_ctrl.h"
#include "hal.h"
#include "mcp4728.h"
#include "nav.h"
#include "rails.h"
#include "settings_store.h"
#include "ssd1306.h"
#include "stream.h"
#include "stream_fmt.h"
#include "ui_model.h"

/* ---- policy constants ---------------------------------------------------- */
#define CONTACT_LOST_END_MS   2000u  /* thumbs off after RUNNING -> summary */
#define NO_CONTACT_DISARM_MS  30000u /* WAIT_CONTACT -> back to the card */
#define MIN_VALID_MS          10000u /* summary needs >= 10 s ... */
#define MIN_VALID_BEATS       5u     /* ... and >= 5 beats / pulses */
#define BATT_POLL_MS          10000u
#define BATT_FAILS_INVALID    3u
#define STANDBY_BATT_WAKE_S   600u   /* timed wake in standby */
#define STATE_EVENT_MS        5000u
#define OVERLAY_USB_MS        2500u
#define OVERLAY_BATT_MS       3000u
#define OVERLAY_FAULT_MS      3000u
#define POWER_HOLD_SHOW_MS    300u
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
    bool power_off_pending;

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
    uint32_t nocontact_ms;  /* since when there is no contact */
    bool ever_running;
    const char *fault_text;
    ui_summary_t summary;
    bool have_session;
    acq_summary_t last;
    bool gain_auto_hi;      /* auto-gain advice remembered for the next record */

    /* battery */
    bool gauge_ok;
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
} S;

static ui_model_t M;
static uint8_t FB[OLED_FB_BYTES];
static char s_fault_buf[64];
static diag_info_t s_diag;

const app_debug_t *app_debug(void) { return &S.dbg; }
const ui_model_t *app_model(void) { return &M; }
const settings_t *app_settings(void) { return &S.settings; }

static void enter_standby(void);
static void start_session(ui_mode_t mode);

/* ======================================================================== */
/* helpers                                                                   */
/* ======================================================================== */

static bool usb_now(void) { return !hal_pin_read(PIN_CHG_PG); }
static bool usb_present(void) { return usb_now() || charger_usb_present(); }
static bool mode_has_ecg(ui_mode_t m) { return m != UI_MODE_PPG; }
static bool mode_has_ppg(ui_mode_t m) { return m != UI_MODE_ECG; }

static void redraw(void) { S.force_draw = true; }

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

static void k_contact(bool on)
{
    stream_ev_lead(on);
    log_line("acq", on ? "contact" : "contact lost");
    if (!on)
        S.nocontact_ms = hal_millis();
    redraw();
}

static void k_settled(bool on)
{
    if (on) {
        /* fresh sweep: the first one must not be scaled to the transient */
        ui_trace_reset(sweep_ms(S.settings.sweep));
        S.ever_running = true;
        log_line("acq", "running");
    }
    redraw();
}

static const acq_sink_t k_sink = {
    stream_ecg_block, stream_ppg_block, k_ecg_trace, k_ppg_trace,
    stream_ev_r_peak, stream_ev_pulse, k_contact, k_settled,
};

/* ======================================================================== */
/* battery                                                                   */
/* ======================================================================== */

static void battery_read(void)
{
    if (!S.gauge_ok) {
        S.gauge_ok = cw2015_init() == HAL_OK;
        S.dbg.gauge_ok = S.gauge_ok;
        if (!S.gauge_ok) {
            S.dbg.batt_valid = false;
            return;
        }
    }
    uint16_t mv = 0;
    uint8_t soc = 0;
    hal_status_t st = cw2015_read(&mv, &soc);
    if (st == HAL_OK || st == HAL_ERR_PARAM) {
        S.batt_fails = 0;
        S.soc_gauge = soc;
        S.dbg.batt_mv = mv;
        S.dbg.batt_pct = batt_pct_combine(mv, soc);
        S.dbg.batt_valid = true;
    } else {
        S.dbg.i2c_errors++;
        if (++S.batt_fails >= BATT_FAILS_INVALID) {
            S.dbg.batt_valid = false;
            S.gauge_ok = false;
            S.dbg.gauge_ok = false;
        }
    }
}

static bool batt_critical(void)
{
    return S.dbg.batt_valid && !usb_present() &&
           (S.dbg.batt_pct < BATT_CUTOFF_PCT || S.dbg.batt_mv < BATT_CUTOFF_MV);
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
    s->duration_s = (uint16_t)(a->running_ms / 1000u);
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
    log_end();
}

/* Every way out of a measurement comes through here (rails_off() inside
 * acq_stop()). Returns true if a summary should be shown. */
static bool end_session(end_t why)
{
    bool was_active = S.ses == SES_ACTIVE;
    acq_stop(); /* stop sequence; also a no-op safety net when idle */
    if (!was_active) {
        S.ses = SES_NONE;
        return false;
    }
    S.ses = SES_NONE;
    S.last = *acq_summary();
    S.have_session = true;
    if (S.settings.gain == GAIN_AUTO && mode_has_ecg(S.mode)) {
        if (S.last.gain_advice > 0)
            S.gain_auto_hi = true;
        else if (S.last.gain_advice < 0)
            S.gain_auto_hi = false;
    }
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

static void start_session(ui_mode_t mode)
{
    S.mode = mode;
    S.ever_running = false;
    S.fault_text = NULL;
    S.nocontact_ms = S.now;
    if (mode_has_ecg(mode) && usb_now()) {
        /* ECG only runs on battery: report, never raise MEASURE_REQ */
        S.ses = SES_USB_BLOCKED;
        go_screen(nav_live_of(mode));
        log_line("session", "blocked: USB power");
        return;
    }
    if (batt_low()) {
        show_overlay(UI_OVERLAY_LOW_BATTERY, OVERLAY_BATT_MS, THEN_NONE, NULL, false);
        log_line("session", "refused: battery low");
        return;
    }
    if (mode_has_ppg(mode) && !S.mcp_ok) {
        S.mcp_ok = mcp4728_init() == HAL_OK; /* one more try */
        S.dbg.mcp_ok = S.mcp_ok;
        if (!S.mcp_ok) {
            S.ses = SES_FAULT;
            S.fault_text = "LED driver MCP4728 not answering: pulse disabled";
            go_screen(nav_live_of(mode));
            log_line("session", "refused: MCP4728 absent");
            return;
        }
    }
    acq_cfg_t c;
    memset(&c, 0, sizeof c);
    c.ecg = mode_has_ecg(mode);
    c.ppg = mode_has_ppg(mode);
    c.gain_hi = S.settings.gain == GAIN_HI || (S.settings.gain == GAIN_AUTO && S.gain_auto_hi);
    c.rld = S.settings.rld;
    c.notch = S.settings.notch;
    c.led_limit_ma = led_limit_ma(S.settings.led_limit);
    if (!acq_start(&c)) {
        S.ses = SES_FAULT;
        S.fault_text = "Could not start acquisition";
        go_screen(nav_live_of(mode));
        return;
    }
    S.ses = SES_ACTIVE;
    S.dbg.sessions++;
    go_screen(nav_live_of(mode));
    log_begin("session");
    log_s(" start mode=");
    log_u((uint32_t)mode);
    log_s(c.gain_hi ? " gain=1098" : " gain=558");
    log_end();
}

static const char *fault_text_of(const acq_status_t *st)
{
    switch (st->err) {
    case ACQ_ERR_VMID: {
        char *p = s_fault_buf, *e = s_fault_buf + sizeof s_fault_buf - 1;
        p = fmt_s(p, e, "VMID ");
        p = fmt_u(p, e, st->vmid_mv);
        p = fmt_s(p, e, " mV, needs 1650+-");
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
    if (st->contact)
        return;
    uint32_t off = S.now - S.nocontact_ms;
    if (S.ever_running) {
        if (off > CONTACT_LOST_END_MS)
            to_summary_or_card(end_session(END_LEAD));
    } else if (off > NO_CONTACT_DISARM_MS) {
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
    if (S.overlay == UI_OVERLAY_BATT_CRITICAL)
        return;
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
            if (S.overlay == UI_OVERLAY_NONE)
                go_screen(UI_SCREEN_HOME);
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
            if (S.overlay == UI_OVERLAY_NONE)
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
        start_session(r->mode);
        break;
    case NAV_SET_CHANGE:
        settings_cycle(&S.settings, S.cursor);
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
    case NAV_DIAG_LEAVE: go_screen(UI_SCREEN_ABOUT); break;
    default: break;
    }
}

static void handle_buttons(void)
{
    btn_event_t e;
    while ((e = buttons_get_event()) != BTN_EV_NONE) {
        S.last_input_ms = S.now;
        if (e == BTN_EV_BTN1_SHORT || e == BTN_EV_BTN2_SHORT || e == BTN_EV_BTN1_LONG ||
            e == BTN_EV_BTN2_LONG)
            stream_ev_button(e == BTN_EV_BTN1_SHORT || e == BTN_EV_BTN1_LONG ? 1u : 2u,
                             e == BTN_EV_BTN1_LONG || e == BTN_EV_BTN2_LONG ? 1u : 0u);
        if (S.power_off_pending || S.dbg.pwr != APP_PWR_AWAKE)
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
    if (buttons_fault() && !S.btn_fault_shown) {
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
        if (!buttons_any_down())
            enter_standby();
        return;
    }
    if (acq_active() || S.overlay != UI_OVERLAY_NONE) {
        /* armed with contact (or about to be): no timeouts */
        S.last_input_ms = S.now;
        return;
    }
    uint32_t idle = S.now - S.last_input_ms;
    if (S.dbg.pwr == APP_PWR_AWAKE && idle >= UI_DISPLAY_TIMEOUT_S * 1000u)
        display_off();
    else if (S.dbg.pwr == APP_PWR_DISPLAY_OFF && idle >= UI_STANDBY_TIMEOUT_S * 1000u)
        enter_standby();
}

static void save_settings_if_needed(bool force)
{
    if (!S.dirty || acq_active() || rails_analog_is_on())
        return;
    if (!force && S.screen == UI_SCREEN_SETTINGS && S.now - S.dirty_ms < SETTINGS_SAVE_IDLE_MS)
        return;
    S.dirty = false;
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
    ssd1306_power_on();
    S.blanked = false;
    S.screen = UI_SCREEN_HOME;
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

static void standby_step(void)
{
    uint32_t rtc = hal_rtc_seconds();
    uint32_t rem = (int32_t)(S.standby_rtc_due - rtc) > 0 ? S.standby_rtc_due - rtc : 1u;
    uint32_t cause = hal_standby(rem);
    hal_wdt_kick();
    if (cause & (HAL_EV_BUTTON | HAL_EV_USB_PLUGGED | HAL_EV_USB_REMOVED)) {
        wake_up(cause);
        return;
    }
    if (cause & HAL_EV_CW_ALRT) {
        (void)cw2015_clear_alert();
        (void)hal_events_take();
    }
    if ((int32_t)(hal_rtc_seconds() - S.standby_rtc_due) >= 0) {
        /* periodic battery check only: display stays off, back to sleep */
        S.dbg.timed_wakes++;
        S.standby_rtc_due = hal_rtc_seconds() + STANDBY_BATT_WAKE_S;
        battery_read();
        (void)hal_events_take();
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
        l->elapsed_s = (uint16_t)(st->running_ms / 1000u);
        l->settle_pct = st->settle_pct;
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
        M.overlay = UI_OVERLAY_POWER_OFF;
        M.hold_progress = 100;
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
                buttons_hold_ms(BTN_1) != 0;
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
    S.dbg.gauge_ok = S.gauge_ok;
    if (!S.gauge_ok)
        log_line("batt", "CW2015 not answering: battery unknown");
    battery_read();
    S.batt_next_ms = S.now + BATT_POLL_MS;

    charger_init();
    charger_tick(S.now);
    buttons_init(S.now);
    stream_init();
    acq_init(&k_sink);
    ui_init();
    ssd1306_power_on();
    S.now = hal_millis();
    S.screen = UI_SCREEN_HOME;
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
    }

    acq_poll(S.now);

    buttons_tick(S.now);
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
