/* HR monitor mode end to end: a standard heart-rate client (LINK_SUB_HR
 * only, like Elite HRV or Zwift) on the simulator's fake CH585. Lifting the
 * thumbs re-arms instead of ending; LINK_STATUS carries mode 4 and every meas
 * change; the display dims and goes dark while the session runs on; a press
 * then only wakes it; the session ends by itself when nobody uses it. */
#include <string.h>

#include "acq.h"
#include "radio.h"
#include "scen.h"
#include "ssd1306.h"
#include "stream_fmt.h"

static bool advertising(void) { return radio_state() == RADIO_ADVERTISING; }
static bool connected(void) { return radio_state() == RADIO_CONNECTED; }
static bool log_has(const char *s) { return strstr(sim_log_text(), s) != NULL; }
static bool hrm_on(void) { return acq_active() && acq_status()->phase == ACQ_ON; }
static bool ecg_running(void) { return acq_status()->ecg_state == ACQ_ECG_RUN; }
static bool ecg_waiting(void) { return acq_status()->ecg_state == ACQ_ECG_WAIT; }
static bool display_dark(void) { return app_debug()->pwr == APP_PWR_DISPLAY_OFF; }
static uint32_t r_peaks(void) { return sim_radio_info()->events_by_kind[STREAM_EV_R_PEAK]; }

/* LINK_STATUS as the radio saw it, followed every main-loop pass: the meas
 * values in order of change (mode 4 throughout once started). */
static struct {
    uint8_t meas[32];
    unsigned n;
    bool other_mode; /* a mode other than 0 / 4 seen */
} T;

static bool track(void)
{
    const uint8_t *st = sim_radio_info()->last_status;
    if (st[1] == LINK_MODE_HRM) {
        if ((T.n == 0 || T.meas[T.n - 1] != st[2]) && T.n < sizeof T.meas)
            T.meas[T.n++] = st[2];
    } else if (st[1] != LINK_MODE_IDLE) {
        T.other_mode = true;
    }
    return false;
}

static void run_track(uint64_t ms) { scen_run_cond(track, ms); }

static bool (*s_until)(void);
static bool track_until(void)
{
    track();
    return s_until();
}
static bool run_track_until(bool (*cond)(void), uint64_t ms)
{
    s_until = cond;
    return scen_run_cond(track_until, ms);
}

/* Home -> the HR monitor card (BUTTON2 x4) */
static void to_hrm_card(void)
{
    scen_run_ms(300);
    for (int i = 0; i < 4; i++)
        scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_HRM_CARD);
}

static void boot_world(void)
{
    scen_boot(false);
    testsrc_cfg_t tc = testsrc_defaults();
    tc.hr_bpm = 64;
    testsrc_install(&tc);
    sim_ecg_set_mains(50.0, 30.0);
    memset(&T, 0, sizeof T);
}

static void end_checks(void)
{
    CHECK_EQ(sim_violation_count(), 0);
    CHECK_EQ(sim_facts()->wdt_resets, 0);
    CHECK_EQ(scen.body_en_outside_session, 0);
    CHECK_EQ(scen.measure_req_on_usb, 0);
    CHECK_EQ(sim_radio_info()->oversize_notify, 0);
    CHECK_EQ(sim_radio_info()->unsolicited, 0);
}

/* ------------------------------------------------------------------------ */

TEST(lift_rearms)
{
    boot_world();
    CHECK(scen_run_cond(advertising, 1000));
    scen_run_ms(50);
    CHECK(sim_phone_connect(247, 16, LINK_SUB_HR)); /* a sports app: Heart Rate only */
    CHECK(scen_run_cond(connected, 100));
    to_hrm_card();
    scen_shot("hrm_card");
    scen_tap(1);
    CHECK_EQ(scen_screen(), UI_SCREEN_HRM);
    CHECK(log_has("session: start mode=4 gain="));
    CHECK(scen_run_cond(hrm_on, 1500));
    run_track(500);
    CHECK_EQ(scen_live_state(), UI_MEAS_WAIT_CONTACT);
    CHECK(scen_m()->ble_hr);
    scen_shot("hrm_wait");

    /* contact: settling, running, R-peaks to the radio (0x2A37) */
    sim_set_thumbs(true);
    CHECK(run_track_until(ecg_running, 8000));
    run_track(10000);
    CHECK_EQ(scen_live_state(), UI_MEAS_RUNNING);
    CHECK_NEAR(scen_m()->live.hr_bpm, 64, 2);
    scen_shot("hrm_running");
    uint32_t rp1 = r_peaks();
    CHECK(rp1 >= 8);

    /* thumbs lifted: back to waiting, not the end, no summary */
    sim_set_thumbs(false);
    CHECK(run_track_until(ecg_waiting, 8000));
    run_track(3000);
    CHECK_EQ(scen_screen(), UI_SCREEN_HRM);
    CHECK_EQ(scen_live_state(), UI_MEAS_WAIT_CONTACT);
    CHECK(acq_active());
    CHECK(!log_has("session: end"));
    CHECK(log_has("ecg: hold -> wait") || log_has("ecg: run -> wait"));
    uint32_t rp_off = r_peaks();
    run_track(5000);
    CHECK_EQ(r_peaks(), rp_off); /* no beats without contact */
    CHECK_EQ(sim_radio_info()->last_status[2], UI_MEAS_WAIT_CONTACT);

    /* thumbs back: settles and runs again, beats flow again */
    sim_set_thumbs(true);
    CHECK(run_track_until(ecg_running, 8000));
    run_track(8000);
    CHECK_EQ(scen_live_state(), UI_MEAS_RUNNING);
    CHECK(r_peaks() >= rp_off + 6);
    CHECK_NEAR(scen_m()->live.hr_bpm, 64, 2);

    /* LINK_STATUS: mode 4 throughout, every meas change seen in order (the
     * lift: "hold still" first, which is SETTLING, then waiting) */
    static const uint8_t want[] = {UI_MEAS_WAIT_CONTACT, UI_MEAS_SETTLING, UI_MEAS_RUNNING,
                                   UI_MEAS_SETTLING,     UI_MEAS_WAIT_CONTACT, UI_MEAS_SETTLING,
                                   UI_MEAS_RUNNING};
    CHECK(!T.other_mode);
    CHECK_EQ(T.n, sizeof want);
    CHECK(memcmp(T.meas, want, sizeof want) == 0);
    /* EV_STATE (to the radio with the HR subscription) says mode 4 */
    bool ev_state4 = false;
    for (size_t i = 0; i < sim_radio_log_count(); i++) {
        const sim_link_frame_t *f = sim_radio_log_get(i);
        if (f->type == STREAM_EVENT && f->len >= 2 && f->p[0] == STREAM_EV_STATE)
            ev_state4 = f->p[1] == LINK_MODE_HRM;
    }
    CHECK(ev_state4);

    /* BUTTON1: the end, back to the card (no summary); the display is
     * dimmed by now (no press for 20 s): the first press only wakes it */
    CHECK_EQ(sim_oled_info().contrast, 0x10);
    scen_tap(1);
    CHECK_EQ(scen_screen(), UI_SCREEN_HRM);
    CHECK(acq_active());
    scen_tap(1);
    CHECK_EQ(scen_screen(), UI_SCREEN_HRM_CARD);
    CHECK(log_has("session: end user"));
    CHECK(!acq_active());
    run_track(600);
    CHECK_EQ(sim_radio_info()->last_status[1], LINK_MODE_IDLE);
    sim_set_thumbs(false);
    end_checks();
}

/* Display: dimmed after HRM_DIM_MS (20 s) without a press, dark after 60 s,
 * the session and the R-peaks run on and the card never sleeps; a press
 * (dimmed or dark) only wakes it, the next one acts. */
TEST(display_dims_and_sleeps)
{
    boot_world();
    to_hrm_card();
    scen_tap(1);
    CHECK(scen_run_cond(hrm_on, 1500));
    sim_set_thumbs(true);
    CHECK(scen_run_cond(ecg_running, 8000));
    CHECK_EQ(sim_oled_info().contrast, SSD1306_CONTRAST_INIT);
    scen_run_ms(18000); /* about 20 s since the press */
    CHECK_EQ(sim_oled_info().contrast, 0x10);
    CHECK(sim_oled_info().display_on);
    CHECK(scen_lit_pixels() > 0);
    CHECK(log_has("power: display dimmed"));

    /* a press while dimmed: brighter, nothing else */
    scen_tap(1);
    scen_run_ms(100);
    CHECK_EQ(sim_oled_info().contrast, SSD1306_CONTRAST_INIT);
    CHECK_EQ(scen_screen(), UI_SCREEN_HRM);
    CHECK(acq_active());

    /* dark after 60 s; the panel sleeps (no power-off: no 100 ms stall) */
    uint64_t t0 = sim_now_ms();
    CHECK(scen_run_cond(display_dark, 62000));
    CHECK(sim_now_ms() - t0 >= 59000);
    CHECK(!sim_oled_info().display_on);
    CHECK(sim_oled_info().powered);
    CHECK_EQ(scen_lit_pixels(), 0);
    CHECK(log_has("power: display dark"));
    uint32_t frames = app_debug()->frames_drawn;
    uint32_t beats = acq_status()->ecg_beats;
    /* long past the display and STANDBY timeouts: still measuring */
    scen_run_ms((UI_STANDBY_TIMEOUT_S + 60u) * 1000u);
    CHECK(app_debug()->pwr == APP_PWR_DISPLAY_OFF);
    CHECK(acq_active() && ecg_running());
    CHECK(acq_status()->ecg_beats > beats + 300);
    CHECK_EQ(app_debug()->frames_drawn, frames); /* nothing drawn in the dark */
    CHECK(scen.max_step_acq_us < 20000);

    /* BUTTON1 in the dark: the display only */
    scen_tap(1);
    scen_run_ms(200);
    CHECK_EQ(app_debug()->pwr, APP_PWR_AWAKE);
    CHECK(sim_oled_info().display_on);
    CHECK_EQ(sim_oled_info().contrast, SSD1306_CONTRAST_INIT);
    CHECK(scen_lit_pixels() > 0);
    CHECK_EQ(scen_screen(), UI_SCREEN_HRM);
    CHECK(acq_active());
    CHECK(!log_has("session: end"));
    scen_shot("hrm_woken");
    /* the next press acts */
    scen_tap(1);
    CHECK_EQ(scen_screen(), UI_SCREEN_HRM_CARD);
    CHECK(!acq_active());
    sim_set_thumbs(false);
    end_checks();
}

/* No contact and no app for 5 min: the end, logged; the advertising window
 * did not expire meanwhile, and counts afresh after the end. */
TEST(auto_end_idle)
{
    boot_world();
    CHECK(scen_run_cond(advertising, 1000));
    to_hrm_card();
    scen_tap(1);
    uint64_t t0 = sim_now_ms();
    scen_run_ms(4u * 60u * 1000u);
    CHECK(acq_active());
    CHECK_EQ(radio_state(), RADIO_ADVERTISING); /* > LINK_ADV_TOTAL_SECONDS */
    CHECK(ecg_waiting());
    while (acq_active() && sim_now_ms() - t0 < 6u * 60u * 1000u)
        scen_run_ms(100);
    CHECK(!acq_active());
    uint64_t dt = sim_now_ms() - t0;
    CHECK(dt >= 5u * 60u * 1000u && dt <= 5u * 60u * 1000u + 500u);
    CHECK(log_has("session: HR monitor: no contact and no app for 5 min"));
    CHECK(log_has("session: end idle"));
    CHECK_EQ(app_debug()->pwr, APP_PWR_DISPLAY_OFF); /* it went dark meanwhile: now off */
    CHECK(!sim_oled_info().powered);
    CHECK(sim_oled_info().dirty_power_offs == 0);
    /* a press shows the card */
    scen_tap(2);
    scen_run_ms(300);
    CHECK_EQ(scen_screen(), UI_SCREEN_HRM_CARD);
    CHECK(scen_lit_pixels() > 0);
    /* the advertising window runs again: off 2 min later, then STANDBY */
    scen_run_ms((LINK_ADV_TOTAL_SECONDS + 5u) * 1000u);
    CHECK_EQ(radio_state(), RADIO_OFF);
    end_checks();
}

/* A client connected: no end at 5 min, but after 30 min without contact;
 * a contact meanwhile restarts the count. */
TEST(auto_end_connected)
{
    boot_world();
    CHECK(scen_run_cond(advertising, 1000));
    scen_run_ms(50);
    CHECK(sim_phone_connect(247, 16, LINK_SUB_HR));
    CHECK(scen_run_cond(connected, 100));
    to_hrm_card();
    scen_tap(1);
    CHECK(scen_run_cond(hrm_on, 1500));
    sim_set_thumbs(true);
    CHECK(scen_run_cond(ecg_running, 8000));
    scen_run_ms(5000);
    sim_set_thumbs(false);
    CHECK(scen_run_cond(ecg_waiting, 8000));
    uint64_t t_off = sim_now_ms();
    scen_run_ms(10u * 60u * 1000u);
    CHECK(acq_active()); /* connected: not idle */
    while (acq_active() && sim_now_ms() - t_off < 31u * 60u * 1000u)
        scen_run_ms(1000);
    CHECK(!acq_active());
    uint64_t dt = sim_now_ms() - t_off;
    printf("  no contact, app connected: end after %llu s\n", (unsigned long long)(dt / 1000u));
    CHECK(dt >= 29u * 60u * 1000u && dt <= 30u * 60u * 1000u + 1000u);
    CHECK(log_has("session: HR monitor: no contact for 30 min"));
    CHECK(log_has("session: end no-contact"));
    CHECK(connected());
    end_checks();
}

/* USB present: blocked like ECG (MEASURE_REQ never raised), mode 4 in
 * LINK_STATUS; unplugged, it arms. */
TEST(usb_blocked)
{
    boot_world();
    sim_set_usb(true);
    scen_run_ms(300);
    to_hrm_card();
    scen_tap(1);
    run_track(600);
    CHECK_EQ(scen_screen(), UI_SCREEN_HRM);
    CHECK_EQ(scen_live_state(), UI_MEAS_USB_BLOCKED);
    CHECK(!acq_active());
    CHECK_EQ(sim_radio_info()->last_status[1], LINK_MODE_HRM);
    CHECK_EQ(sim_radio_info()->last_status[2], UI_MEAS_USB_BLOCKED);
    sim_set_usb(false);
    CHECK(scen_run_cond(hrm_on, 2000));
    CHECK_EQ(scen_screen(), UI_SCREEN_HRM);
    scen_press(2, 2200); /* BUTTON2 long: Home, as everywhere */
    CHECK_EQ(scen_screen(), UI_SCREEN_HOME);
    CHECK(!acq_active());
    CHECK(log_has("session: end abort"));
    end_checks();
}

int main(void)
{
    RUN(lift_rearms);
    RUN(display_dims_and_sleeps);
    RUN(auto_end_idle);
    RUN(auto_end_connected);
    RUN(usb_blocked);
    return TEST_RESULT();
}
