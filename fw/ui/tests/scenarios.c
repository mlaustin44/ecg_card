#include "scenarios.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

static const double PI_ = 3.14159265358979323846;

/* ---- signal simulation ---------------------------------------------------- */

static struct {
    sim_sig_t sig;
    double t;          /* seconds since sim_begin */
    double r[4]; /* beat times, r[1] <= t < r[2] */
    uint32_t ecg_n, ppg_n;
    uint32_t lcg;
    uint32_t t0_ms;
    double run_s;
} sim_;

static double noise1(void)
{
    sim_.lcg = sim_.lcg * 1664525u + 1013904223u;
    return ((double)(sim_.lcg >> 8) / (double)(1u << 24)) * 2.0 - 1.0;
}

static double gss(double x, double mu, double s)
{
    double z = (x - mu) / s;
    return exp(-0.5 * z * z);
}

static double ecg_beat(double dt) /* dt from the R peak, s; amplitude 1 = R */
{
    return 0.11 * gss(dt, -0.17, 0.022) - 0.10 * gss(dt, -0.028, 0.008) +
           1.00 * gss(dt, 0.0, 0.0095) - 0.24 * gss(dt, 0.026, 0.009) +
           0.28 * gss(dt, 0.26, 0.045) + 0.02 * gss(dt, 0.43, 0.03);
}

static double ppg_beat(double dt) /* dt from the pulse foot, s */
{
    if (dt < -0.05)
        return 0.0;
    return gss(dt, 0.13, 0.055) + 0.42 * gss(dt, 0.36, 0.085);
}

static double rr_at(double t)
{
    double rr = 60.0 / (double)(sim_.sig.hr_bpm ? sim_.sig.hr_bpm : 60u);
    return rr * (1.0 + 0.04 * sin(2.0 * PI_ * t / 4.2)); /* sinus arrhythmia */
}

void sim_set(const sim_sig_t *sig) { sim_.sig = *sig; }

void sim_begin(ui_model_t *m, const sim_sig_t *sig)
{
    memset(&sim_, 0, sizeof sim_);
    sim_.sig = *sig;
    sim_.lcg = 12345u;
    sim_.r[2] = 0.35;
    for (int i = 1; i >= 0; i--)
        sim_.r[i] = sim_.r[i + 1] - rr_at(0.0);
    sim_.r[3] = sim_.r[2] + rr_at(sim_.r[2]);
    sim_.t0_ms = m->now_ms;
    ui_trace_reset(sweep_ms(m->settings.sweep));
}

static void beats_to(double t)
{
    while (t >= sim_.r[2]) {
        for (int i = 0; i < 3; i++)
            sim_.r[i] = sim_.r[i + 1];
        sim_.r[3] = sim_.r[2] + rr_at(sim_.r[2]);
    }
}

static bool beat_crossed(double t0, double t1, double off)
{
    for (int i = 0; i < 4; i++)
        if (sim_.r[i] + off > t0 && sim_.r[i] + off <= t1)
            return true;
    return false;
}

void sim_run(ui_model_t *m, uint32_t ms)
{
    bool ecg = m->screen != UI_SCREEN_PPG;
    bool ppg = m->screen != UI_SCREEN_ECG;
    double pat = sim_.sig.pat_ms / 1000.0;
    for (uint32_t done = 0; done < ms; done += 40u) {
        uint32_t step = ms - done < 40u ? ms - done : 40u;
        double t0 = sim_.t, t1 = sim_.t + step / 1000.0;
        int16_t eb[64], pb[16];
        uint16_t ne = 0, np = 0;
        /* ECG samples due in (t0, t1] */
        while (ecg && (double)(sim_.ecg_n + 1) / ECG_HZ <= t1 + 1e-9 && ne < 64) {
            double t = (double)(++sim_.ecg_n) / ECG_HZ;
            beats_to(t);
            double v = 0.0;
            for (int i = 0; i < 4; i++)
                v += ecg_beat(t - sim_.r[i]);
            v *= sim_.sig.ecg_uv;
            v += sim_.sig.wander_uv * sin(2.0 * PI_ * 0.27 * t);
            v += sim_.sig.mains_uv * sin(2.0 * PI_ * 50.0 * t);
            v += sim_.sig.noise_uv * noise1();
            if (v > 32767)
                v = 32767;
            if (v < -32768)
                v = -32768;
            eb[ne++] = (int16_t)lrint(v);
        }
        while (ppg && (double)(sim_.ppg_n + 1) / PPG_HZ <= t1 + 1e-9 && np < 16) {
            double t = (double)(++sim_.ppg_n) / PPG_HZ;
            beats_to(t);
            double v = 0.0;
            for (int i = 0; i < 4; i++)
                v += ppg_beat(t - sim_.r[i] - pat);
            v = sim_.sig.ppg_amp * (v - 0.45);
            v += 0.15 * sim_.sig.ppg_amp * sin(2.0 * PI_ * 0.22 * t);
            v += 0.02 * sim_.sig.ppg_amp * noise1();
            pb[np++] = (int16_t)lrint(v);
        }
        if (ne)
            ui_trace_push(UI_TRACE_ECG, eb, ne);
        if (np)
            ui_trace_push(UI_TRACE_PPG, pb, np);
        if (sim_.sig.hr_bpm && beat_crossed(t0, t1, ecg ? 0.0 : pat))
            m->live.beat_seq++;
        sim_.t = t1;
        m->now_ms = sim_.t0_ms + (uint32_t)lrint(sim_.t * 1000.0);
        if (m->live.state == UI_MEAS_RUNNING)
            m->live.elapsed_s = (uint16_t)(sim_.t);
    }
}

/* ---- scenarios -------------------------------------------------------------- */

void scen_defaults(ui_model_t *m)
{
    static const settings_t defs = SETTINGS_DEFAULTS;
    memset(m, 0, sizeof *m);
    ui_init();
    m->screen = UI_SCREEN_HOME;
    m->now_ms = 100000u;
    m->batt_pct = 76;
    m->batt_valid = true;
    m->batt_mv = 3912;
    m->settings = defs;
    m->fw_version = "0.1.0";
    m->live.notch_hz = 50;
}

static const sim_sig_t SIG_NORMAL = {72, 1000, 1500, 210, 12, 60, 8};
static const sim_sig_t SIG_FAST = {205, 420, 900, 150, 10, 30, 6};
static const sim_sig_t SIG_BIG = {58, 2600, 3000, 240, 15, 80, 10};
static const sim_sig_t SIG_TINY = {64, 180, 300, 230, 6, 20, 5};

static void live(ui_model_t *m, ui_screen_t s, ui_meas_state_t st)
{
    m->screen = s;
    m->live.state = st;
    m->live.quality = 4;
}

static void run_live(ui_model_t *m, ui_screen_t s, const sim_sig_t *sig, uint32_t ms)
{
    live(m, s, UI_MEAS_RUNNING);
    m->live.hr_bpm = s != UI_SCREEN_PPG ? (uint16_t)sig->hr_bpm : 0;
    m->live.pulse_bpm = s != UI_SCREEN_ECG ? (uint16_t)sig->hr_bpm : 0;
    m->live.pat_ms = s == UI_SCREEN_BOTH ? (uint16_t)sig->pat_ms : 0;
    m->live.pi_permille = s != UI_SCREEN_ECG ? 23 : 0;
    sim_begin(m, sig);
    sim_run(m, ms);
}

#define SC(name) static void sc_##name(ui_model_t *m)

SC(home) { scen_defaults(m); }
SC(home_charging) { scen_defaults(m); m->usb_present = m->charging = true; m->batt_pct = 43; }
SC(home_charged) { scen_defaults(m); m->usb_present = true; m->batt_pct = 100; }
SC(home_fault) { scen_defaults(m); m->usb_present = m->charge_fault = true; m->batt_pct = 61; }
SC(home_low) { scen_defaults(m); m->batt_pct = 4; }
SC(home_batt0) { scen_defaults(m); m->batt_pct = 0; }
SC(home_unknown) { scen_defaults(m); m->batt_valid = false; m->batt_pct = 0; }
SC(home_time) { scen_defaults(m); m->time_valid = true; m->hour = 9; m->minute = 41; }
SC(home_ble_adv) { scen_defaults(m); m->ble_state = UI_BLE_ADVERTISING; }
SC(home_ble_adv_dark) { scen_defaults(m); m->ble_state = UI_BLE_ADVERTISING; m->now_ms = 100700u; }
SC(home_ble_conn)
{
    scen_defaults(m);
    m->ble_state = UI_BLE_CONNECTED;
    m->time_valid = true;
    m->hour = 10;
    m->minute = 41;
}
SC(home_ble_fault) { scen_defaults(m); m->ble_state = UI_BLE_FAULT; }

SC(card_ecg) { scen_defaults(m); m->screen = UI_SCREEN_ECG_CARD; }
SC(card_ecg_usb) { scen_defaults(m); m->screen = UI_SCREEN_ECG_CARD; m->usb_present = m->charging = true; }
SC(card_ecg_low) { scen_defaults(m); m->screen = UI_SCREEN_ECG_CARD; m->batt_pct = 3; }
SC(card_ppg) { scen_defaults(m); m->screen = UI_SCREEN_PPG_CARD; }
SC(card_ppg_usb) { scen_defaults(m); m->screen = UI_SCREEN_PPG_CARD; m->usb_present = true; m->batt_pct = 100; }
SC(card_both) { scen_defaults(m); m->screen = UI_SCREEN_BOTH_CARD; }
SC(card_ecg_ble_conn) { scen_defaults(m); m->screen = UI_SCREEN_ECG_CARD; m->ble_state = UI_BLE_CONNECTED; }
SC(card_ppg_ble_fault_usb)
{
    scen_defaults(m);
    m->screen = UI_SCREEN_PPG_CARD;
    m->ble_state = UI_BLE_FAULT;
    m->usb_present = m->charging = true;
}

static void settings_at(ui_model_t *m, uint8_t cur)
{
    scen_defaults(m);
    m->screen = UI_SCREEN_SETTINGS;
    m->settings_cursor = cur;
}
SC(settings_0) { settings_at(m, 0); }
SC(settings_1) { settings_at(m, 1); }
SC(settings_2) { settings_at(m, 2); }
SC(settings_3) { settings_at(m, 3); }
SC(settings_4) { settings_at(m, 4); }
SC(settings_5) { settings_at(m, 5); m->ble_state = UI_BLE_ADVERTISING; }
SC(settings_5_off) { settings_at(m, 5); m->settings.ble = BLE_OFF; }
SC(settings_6) { settings_at(m, 6); m->settings.restore = RESTORE_SLOW; }
SC(settings_7) { settings_at(m, 7); m->settings.view = VIEW_HR_GRAPH; }
SC(settings_alt)
{
    settings_at(m, 2);
    m->settings.gain = GAIN_HI;
    m->settings.notch = NOTCH_60;
    m->settings.rld = RLD_RIGHT;
    m->settings.led_limit = LED_LIMIT_5MA;
    m->settings.sweep = SWEEP_1S5;
}
SC(about) { scen_defaults(m); m->screen = UI_SCREEN_ABOUT; }
SC(about_radio)
{
    scen_defaults(m);
    m->screen = UI_SCREEN_ABOUT;
    m->ble_state = UI_BLE_CONNECTED;
    m->radio_fw = "1.2.3";
    m->ble_id = "E5F6";
}
SC(diag)
{
    static const char *const lines[UI_DIAG_LINES] = {
        "VMID 1651mV  ok", "ECG mid 2049 rms 3", "PPG dark 112 on 2310",
        "LED 9.3mA dac 2501", "CW2015 3912mV 76%", "I2C err 0  wdt 0",
    };
    scen_defaults(m);
    m->screen = UI_SCREEN_DIAG;
    m->diag_page = 1;
    m->diag_pages = 4;
    for (int i = 0; i < UI_DIAG_LINES; i++)
        strncpy(m->diag[i], lines[i], UI_DIAG_COLS);
}
SC(diag_full)
{
    scen_defaults(m);
    m->screen = UI_SCREEN_DIAG;
    m->diag_page = 9;
    m->diag_pages = 10;
    for (int i = 0; i < UI_DIAG_LINES; i++)
        memset(m->diag[i], i & 1 ? 'W' : '8', UI_DIAG_COLS);
}

SC(ecg_wait_a) { scen_defaults(m); live(m, UI_SCREEN_ECG, UI_MEAS_WAIT_CONTACT); m->live.quality = 0; m->now_ms = 100100u; }
SC(ecg_wait_b) { scen_defaults(m); live(m, UI_SCREEN_ECG, UI_MEAS_WAIT_CONTACT); m->live.quality = 0; m->now_ms = 100300u; }
SC(ecg_wait_c) { scen_defaults(m); live(m, UI_SCREEN_ECG, UI_MEAS_WAIT_CONTACT); m->live.quality = 0; m->now_ms = 101000u; }
SC(ppg_wait) { scen_defaults(m); live(m, UI_SCREEN_PPG, UI_MEAS_WAIT_CONTACT); m->live.quality = 0; m->now_ms = 101000u; }
SC(both_wait) { scen_defaults(m); live(m, UI_SCREEN_BOTH, UI_MEAS_WAIT_CONTACT); m->live.quality = 0; m->now_ms = 100250u; }
SC(ecg_settling) { scen_defaults(m); live(m, UI_SCREEN_ECG, UI_MEAS_SETTLING); m->live.settle_pct = 40; m->live.quality = 2; }
SC(ecg_wait_hum) { scen_defaults(m); live(m, UI_SCREEN_ECG, UI_MEAS_WAIT_CONTACT); m->live.quality = 0; m->live.hum = true; m->now_ms = 101000u; }
SC(ecg_settling_hum) { scen_defaults(m); live(m, UI_SCREEN_ECG, UI_MEAS_SETTLING); m->live.settle_pct = 40; m->live.quality = 1; m->live.hum = true; }
SC(ecg_settling_0) { scen_defaults(m); live(m, UI_SCREEN_ECG, UI_MEAS_SETTLING); m->live.settle_pct = 0; m->live.quality = 0; }
SC(ppg_settling) { scen_defaults(m); live(m, UI_SCREEN_PPG, UI_MEAS_SETTLING); m->live.settle_pct = 100; m->live.quality = 3; }
SC(ecg_usb) { scen_defaults(m); live(m, UI_SCREEN_ECG, UI_MEAS_USB_BLOCKED); m->usb_present = m->charging = true; }
SC(both_usb) { scen_defaults(m); live(m, UI_SCREEN_BOTH, UI_MEAS_USB_BLOCKED); m->usb_present = true; m->batt_pct = 100; }
SC(ecg_fault)
{
    scen_defaults(m);
    live(m, UI_SCREEN_ECG, UI_MEAS_FAULT);
    m->fault_text = "VMID out of range: 1712 mV (expected 1650 +/- 10 mV). Check ANA_EN rail.";
}
SC(ecg_fault_null) { scen_defaults(m); live(m, UI_SCREEN_ECG, UI_MEAS_FAULT); }

SC(ecg_run) { scen_defaults(m); run_live(m, UI_SCREEN_ECG, &SIG_NORMAL, 7300); }
SC(ecg_run_205)
{
    scen_defaults(m);
    run_live(m, UI_SCREEN_ECG, &SIG_FAST, 6600);
    m->live.gain_hi = true;
    m->live.quality = 3;
}
/* Live view "+HR graph": 32 s of a 70 +- 6 bpm rate breathing every 5 s (RSA) */
static void rsa_graph(void)
{
    ui_hrgraph_reset();
    for (unsigned k = 0; k < UI_HRG_N; k++)
        ui_hrgraph_push((uint16_t)lrint(70.0 + 6.0 * sin(2.0 * PI_ * k * UI_HRG_STEP_MS / 5000.0)));
}
SC(ecg_run_hr) { scen_defaults(m); m->settings.view = VIEW_HR_GRAPH; run_live(m, UI_SCREEN_ECG, &SIG_NORMAL, 7300); rsa_graph(); m->live.fast_bpm = 74; }
SC(ppg_run_hr) { scen_defaults(m); m->settings.view = VIEW_HR_GRAPH; run_live(m, UI_SCREEN_PPG, &SIG_NORMAL, 7300); rsa_graph(); m->live.fast_bpm = 66; }
SC(ecg_run_hr_start) { scen_defaults(m); m->settings.view = VIEW_HR_GRAPH; run_live(m, UI_SCREEN_ECG, &SIG_NORMAL, 7300); ui_hrgraph_reset(); for (int k = 0; k < 20; k++) ui_hrgraph_push(k < 6 ? 0 : 72); m->live.fast_bpm = 0; }
SC(ecg_run_big) { scen_defaults(m); run_live(m, UI_SCREEN_ECG, &SIG_BIG, 9500); m->usb_present = false; }
SC(ecg_run_tiny)
{
    scen_defaults(m);
    run_live(m, UI_SCREEN_ECG, &SIG_TINY, 13100);
    m->live.gain_hi = true;
    m->live.quality = 1;
    m->batt_pct = 8;
}
SC(ecg_run_start)
{
    scen_defaults(m);
    run_live(m, UI_SCREEN_ECG, &SIG_NORMAL, 1700);
    m->live.hr_bpm = 0;
    m->live.quality = 2;
}
SC(ecg_run_1s5) { scen_defaults(m); m->settings.sweep = SWEEP_1S5; run_live(m, UI_SCREEN_ECG, &SIG_NORMAL, 4100); }
SC(ecg_run_6s) { scen_defaults(m); m->settings.sweep = SWEEP_6S; run_live(m, UI_SCREEN_ECG, &SIG_NORMAL, 9000); }
SC(ppg_run) { scen_defaults(m); run_live(m, UI_SCREEN_PPG, &SIG_NORMAL, 7300); }
SC(ppg_run_start)
{
    scen_defaults(m);
    run_live(m, UI_SCREEN_PPG, &SIG_NORMAL, 1200);
    m->live.pulse_bpm = 0;
    m->live.pi_permille = 0;
    m->live.quality = 1;
}
SC(both_run) { scen_defaults(m); run_live(m, UI_SCREEN_BOTH, &SIG_NORMAL, 7300); }
SC(both_run_205) { scen_defaults(m); run_live(m, UI_SCREEN_BOTH, &SIG_FAST, 7300); m->live.quality = 2; }
SC(both_run_start)
{
    scen_defaults(m);
    run_live(m, UI_SCREEN_BOTH, &SIG_NORMAL, 900);
    m->live.hr_bpm = m->live.pulse_bpm = m->live.pat_ms = 0;
    m->live.quality = 1;
}

static void summary(ui_model_t *m, ui_mode_t mode)
{
    scen_defaults(m);
    m->screen = UI_SCREEN_SUMMARY;
    ui_summary_t *s = &m->summary;
    s->mode = mode;
    s->valid = true;
    s->duration_s = 65;
    if (mode != UI_MODE_PPG) {
        s->beats = 78;
        s->hr_avg = 72;
        s->hr_min = 58;
        s->hr_max = 91;
        s->sdnn_ms = 42;
        s->rmssd_ms = 35;
    }
    if (mode != UI_MODE_ECG) {
        s->pulse_avg = 71;
        s->pi_permille = 23;
    }
    if (mode == UI_MODE_BOTH)
        s->pat_ms = 214;
}
SC(sum_ecg) { summary(m, UI_MODE_ECG); }
SC(sum_ecg_usb) { summary(m, UI_MODE_ECG); m->summary.stopped_by_usb = true; m->summary.duration_s = 23; }
SC(sum_ppg) { summary(m, UI_MODE_PPG); }
SC(sum_both) { summary(m, UI_MODE_BOTH); }
SC(sum_both_max)
{
    summary(m, UI_MODE_BOTH);
    ui_summary_t *s = &m->summary;
    s->hr_avg = 188; s->hr_min = 102; s->hr_max = 205; s->beats = 9999;
    s->sdnn_ms = 312; s->rmssd_ms = 1250; s->pat_ms = 999; s->pi_permille = 199;
    s->pulse_avg = 187; s->duration_s = 3599; s->stopped_by_usb = true;
}
SC(sum_both_na)
{
    summary(m, UI_MODE_BOTH);
    ui_summary_t *s = &m->summary;
    s->sdnn_ms = s->rmssd_ms = s->pat_ms = s->pi_permille = s->pulse_avg = 0;
    s->duration_s = 14;
    s->beats = 15;
}
SC(sum_invalid) { summary(m, UI_MODE_ECG); m->summary.valid = false; m->summary.duration_s = 4; }

SC(ov_usb) { sc_ecg_run(m); m->overlay = UI_OVERLAY_USB_STOPPED; m->usb_present = m->charging = true; }
SC(ov_lowbatt) { sc_card_ecg(m); m->overlay = UI_OVERLAY_LOW_BATTERY; m->batt_pct = 4; }
SC(ov_critical) { scen_defaults(m); m->overlay = UI_OVERLAY_BATT_CRITICAL; m->batt_pct = 2; }
SC(ov_power_0) { scen_defaults(m); m->overlay = UI_OVERLAY_POWER_OFF; m->hold_progress = 0; }
SC(ov_power_50) { sc_ecg_run(m); m->overlay = UI_OVERLAY_POWER_OFF; m->hold_progress = 50; }
SC(ov_power_100) { scen_defaults(m); m->overlay = UI_OVERLAY_POWER_OFF; m->hold_progress = 100; }
SC(ov_fault)
{
    sc_settings_1(m);
    m->overlay = UI_OVERLAY_FAULT;
    m->fault_text = "Fuel gauge not responding on I2C (addr 0x62, NACK). Battery level unknown; "
                    "measurements disabled until restart.";
}
SC(ov_fault_short) { sc_both_run(m); m->overlay = UI_OVERLAY_FAULT; m->fault_text = "PPG LED open"; }
SC(ov_fault_null) { scen_defaults(m); m->overlay = UI_OVERLAY_FAULT; }

static void splash(ui_model_t *m, ui_screen_t s, uint16_t ms)
{
    scen_defaults(m);
    m->screen = s;
    m->screen_ms = ms;
    m->now_ms += ms;
}
SC(boot) { splash(m, UI_SCREEN_BOOT, 1500); }
SC(boot_t0) { splash(m, UI_SCREEN_BOOT, 0); }
SC(boot_t250) { splash(m, UI_SCREEN_BOOT, 250); }
SC(boot_t500) { splash(m, UI_SCREEN_BOOT, 500); }
SC(boot_t750) { splash(m, UI_SCREEN_BOOT, 750); }
SC(boot_nover) { splash(m, UI_SCREEN_BOOT, 1500); m->fw_version = NULL; }
SC(power_down) { splash(m, UI_SCREEN_POWER_DOWN, 0); }
SC(power_down_end) { splash(m, UI_SCREEN_POWER_DOWN, 1400); }

#define E(name) {#name, sc_##name}
const scenario_t scenarios[] = {
    E(home), E(home_charging), E(home_charged), E(home_fault), E(home_low), E(home_batt0),
    E(home_unknown), E(home_time), E(home_ble_adv), E(home_ble_adv_dark), E(home_ble_conn),
    E(home_ble_fault),
    E(card_ecg), E(card_ecg_usb), E(card_ecg_low), E(card_ppg), E(card_ppg_usb), E(card_both),
    E(card_ecg_ble_conn), E(card_ppg_ble_fault_usb),
    E(settings_0), E(settings_1), E(settings_2), E(settings_3), E(settings_4), E(settings_5),
    E(settings_5_off), E(settings_6), E(settings_7), E(settings_alt),
    E(about), E(about_radio), E(diag), E(diag_full),
    E(ecg_wait_a), E(ecg_wait_b), E(ecg_wait_c), E(ppg_wait), E(both_wait),
    E(ecg_settling), E(ecg_settling_0), E(ecg_wait_hum), E(ecg_settling_hum), E(ppg_settling),
    E(ecg_usb), E(both_usb), E(ecg_fault), E(ecg_fault_null),
    E(ecg_run), E(ecg_run_hr), E(ppg_run_hr), E(ecg_run_hr_start), E(ecg_run_205), E(ecg_run_big), E(ecg_run_tiny), E(ecg_run_start),
    E(ecg_run_1s5), E(ecg_run_6s),
    E(ppg_run), E(ppg_run_start), E(both_run), E(both_run_205), E(both_run_start),
    E(sum_ecg), E(sum_ecg_usb), E(sum_ppg), E(sum_both), E(sum_both_max), E(sum_both_na),
    E(sum_invalid),
    E(ov_usb), E(ov_lowbatt), E(ov_critical), E(ov_power_0), E(ov_power_50), E(ov_power_100),
    E(ov_fault), E(ov_fault_short), E(ov_fault_null),
    E(boot), E(boot_t0), E(boot_t250), E(boot_t500), E(boot_t750), E(boot_nover),
    E(power_down), E(power_down_end),
};
const int n_scenarios = (int)(sizeof scenarios / sizeof scenarios[0]);

const scenario_t *scenario_find(const char *name)
{
    for (int i = 0; i < n_scenarios; i++)
        if (strcmp(scenarios[i].name, name) == 0)
            return &scenarios[i];
    return NULL;
}
