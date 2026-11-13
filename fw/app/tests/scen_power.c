/* Power policy: display / standby timeouts, wake, power-off, battery. */
#include <string.h>

#include "acq.h"
#include "buttons.h"
#include "scen.h"

static bool in_standby(void) { return app_debug()->pwr == APP_PWR_STANDBY; }
#define OVERLAY_FAULT_MS_T 3000u /* app.c OVERLAY_FAULT_MS */
static void radio_irq_pulse(void *ctx)
{
    (void)ctx;
    sim_set_radio_irq(true);
    sim_set_radio_irq(false);
}
static bool display_off(void) { return app_debug()->pwr == APP_PWR_DISPLAY_OFF; }
static bool awake(void) { return app_debug()->pwr == APP_PWR_AWAKE; }

static bool all_rails_off(void)
{
    return scen_analog_all_off() && scen_radio_off() && !sim_pin(PIN_OLED_EN) && !sim_oled_bus_attached() &&
           !sim_pin(PIN_OLED_DC) && !sim_pin(PIN_OLED_CS) && !sim_pin(PIN_OLED_RESET);
}

/* MCP4728 internal reference (~45 uA on the always-on rail): never on in
 * STANDBY, never on outside a PPG session. */
static uint64_t s_vref_idle0; /* reference time the test set up itself (old EEPROM) */
static void check_vref_off(void)
{
    CHECK_EQ(sim_facts()->mcp_vref_int_standby, 0);
    CHECK_EQ(sim_facts()->mcp_vref_int_idle_us - s_vref_idle0, 0);
    CHECK_EQ(scen.vref_outside_ppg, 0);
    if (in_standby())
        CHECK(!sim_mcp_int_ref_on());
}

TEST(timeouts_and_wake)
{
    scen_boot(false);
    scen_run_ms(300);
    scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG_CARD);
    uint64_t t_input = sim_now_ms();
    scen_run_until(t_input + UI_DISPLAY_TIMEOUT_S * 1000 - 500);
    CHECK(awake());
    CHECK(sim_oled_info().powered);
    CHECK(scen_run_cond(display_off, 1000));
    CHECK(!sim_oled_info().powered);
    CHECK_EQ(sim_oled_info().dirty_power_offs, 0); /* AEh, 8D 10, wait, then rail */
    /* any button wakes the display without acting */
    scen_tap(2);
    scen_run_ms(300);
    CHECK(awake());
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG_CARD);
    CHECK(scen_lit_pixels() > 100);
    /* standby after UI_STANDBY_TIMEOUT_S without input */
    t_input = sim_now_ms();
    CHECK(scen_run_cond(in_standby, UI_STANDBY_TIMEOUT_S * 1000 + 2000));
    uint64_t dt = sim_now_ms() - t_input;
    printf("  standby after %llu ms idle\n", (unsigned long long)dt);
    scen_run_ms(10);
    CHECK(dt >= UI_STANDBY_TIMEOUT_S * 1000 - 500 && dt <= UI_STANDBY_TIMEOUT_S * 1000 + 300);
    CHECK(all_rails_off());
    CHECK_EQ(sim_facts()->standby_entries, 1);
    /* wake by BUTTON2: Home, and the press does not move to the ECG card */
    scen_run_ms(5000);
    scen_tap(2);
    scen_run_ms(400);
    CHECK(awake());
    CHECK_EQ(scen_screen(), UI_SCREEN_HOME);
    CHECK(scen_lit_pixels() > 100);
    scen_shot("wake_home");
    /* holding BUTTON1 through a wake does not power off */
    scen_run_ms(UI_DISPLAY_TIMEOUT_S * 1000 + UI_STANDBY_TIMEOUT_S * 1000);
    CHECK(in_standby());
    scen_press(1, 3000);
    scen_run_ms(500);
    CHECK(awake());
    CHECK_EQ(scen_m()->overlay, UI_OVERLAY_NONE);
    CHECK_EQ(sim_facts()->wdt_resets, 0);
    check_vref_off();
}

TEST(power_off_long_press)
{
    scen_boot(false);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    scen_run_ms(300);
    /* in the middle of a PPG measurement */
    scen_tap(2);
    scen_tap(2);
    sim_set_finger(true);
    scen_tap(1);
    scen_run_ms(4000);
    CHECK(acq_active());
    uint64_t t = sim_now_ms() + 1;
    sim_at_button(t, 1, true);
    sim_at_button(t + 2600, 1, false);
    scen_run_until(t + 1200);
    CHECK_EQ(scen_m()->overlay, UI_OVERLAY_POWER_OFF);
    CHECK(scen_m()->hold_progress > 50 && scen_m()->hold_progress < 70);
    scen_shot("power_off_hold");
    scen_run_until(t + 2300);
    CHECK(!acq_active());                 /* long press: measurement stopped */
    CHECK(scen_analog_all_off());
    CHECK(awake());                       /* waits for the release */
    CHECK_EQ(scen_m()->hold_progress, 100);
    scen_shot("power_off_release");
    scen_run_until(t + 2800);
    CHECK(in_standby());
    CHECK(all_rails_off());
    CHECK(!sim_mcp_int_ref_on()); /* channel A was on the internal reference */
    /* the release edge did not wake it again */
    scen_run_ms(20000);
    CHECK(in_standby());
    CHECK_EQ(app_debug()->wakes, 0);
    check_vref_off();
    sim_set_finger(false);
}

static bool ppg_running(void) { return acq_active() && acq_status()->phase == ACQ_ON; }

/* A part programmed by an earlier build (channel A EEPROM on the internal
 * reference) is re-programmed once; after that, every way out of a PPG
 * session and every standby leaves the reference off. */
TEST(mcp4728_reference_off)
{
    sim_reset();
    sim_set_violation_mode(SIM_VIOL_ABORT);
    sim_mcp_eeprom_set(0, 0, true, 1, false);
    for (int ch = 1; ch < 4; ch++)
        sim_mcp_eeprom_set(ch, 0, false, 1, false);
    sim_mcp_power_cycle();
    CHECK(sim_mcp_int_ref_on()); /* as loaded from the old EEPROM */
    memset(&scen, 0, sizeof scen);
    app_init();
    CHECK(!sim_mcp_int_ref_on());
    s_vref_idle0 = sim_facts()->mcp_vref_int_idle_us; /* boot until mcp4728_init() */
    CHECK(s_vref_idle0 < 5000);
    CHECK_EQ(sim_facts()->mcp_eeprom_writes, 4); /* one Sequential Write, A..D */
    for (int ch = 0; ch < 4; ch++) {
        bool vref;
        unsigned pd;
        sim_mcp_eeprom_get(ch, NULL, &vref, &pd, NULL);
        CHECK(!vref && pd == 1);
    }
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    scen_run_ms(300);
    /* PPG session: the reference runs while channel A is on */
    scen_tap(2);
    scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_PPG_CARD);
    sim_set_finger(true);
    scen_tap(1);
    CHECK(scen_run_cond(ppg_running, 2000));
    scen_run_ms(3000);
    CHECK(sim_mcp_int_ref_on());
    scen_tap(1); /* end: summary */
    scen_run_ms(200);
    CHECK(!acq_active());
    CHECK(!sim_mcp_int_ref_on());
    /* idle until display-off and standby: the reference stays off */
    scen_tap(2);
    scen_run_ms(200);
    sim_set_finger(false);
    scen_run_ms(UI_DISPLAY_TIMEOUT_S * 1000 + UI_STANDBY_TIMEOUT_S * 1000 + 2000);
    CHECK(in_standby());
    check_vref_off();
    /* reboot: EEPROM already right, not written again */
    scen_reboot();
    scen_run_ms(500);
    CHECK_EQ(sim_facts()->mcp_eeprom_writes, 4);
    check_vref_off();
    s_vref_idle0 = 0;
}

TEST(periodic_battery_wake)
{
    scen_boot(false);
    scen_run_ms(300);
    scen_press(1, 2200);
    scen_run_ms(200);
    CHECK(in_standby());
    uint32_t xfers = sim_facts()->i2c_xfers;
    scen_run_ms(25ull * 60 * 1000); /* 25 minutes */
    CHECK(in_standby());
    CHECK(app_debug()->timed_wakes >= 2 && app_debug()->timed_wakes <= 3);
    CHECK(!sim_oled_info().powered);
    CHECK_EQ(sim_oled_info().power_ups, 1);
    uint32_t per_wake = (sim_facts()->i2c_xfers - xfers) / app_debug()->timed_wakes;
    CHECK(per_wake <= 2); /* cheap: one gauge read */
    /* battery fell while asleep: the next timed wake sees it */
    sim_set_battery_mv(3600);
    scen_run_ms(11ull * 60 * 1000);
    CHECK_EQ(app_debug()->batt_mv / 10, 359);
    CHECK(in_standby());
    /* USB plugged wakes to Home */
    sim_set_usb(true);
    scen_run_ms(500);
    CHECK(awake());
    CHECK_EQ(scen_screen(), UI_SCREEN_HOME);
    CHECK(scen_m()->usb_present);
}

static void set_batt(uint16_t mv)
{
    sim_set_battery_mv(mv);
}

TEST(low_battery)
{
    scen_boot(false);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    set_batt(3480); /* ~3 %: below BATT_WARN_PCT, above the cut-off */
    scen_run_ms(300);
    CHECK(scen_m()->batt_valid && scen_m()->batt_pct < BATT_WARN_PCT &&
          scen_m()->batt_pct >= BATT_CUTOFF_PCT);
    scen_shot("home_battery_low");
    scen_tap(2);
    scen_shot("ecg_card_battery_low");
    scen_tap(1);
    scen_run_ms(200);
    CHECK_EQ(scen_m()->overlay, UI_OVERLAY_LOW_BATTERY);
    CHECK(scen_analog_all_off());
    scen_shot("low_battery_overlay");
    scen_run_ms(3500);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG_CARD);
    CHECK(!acq_active());
    /* on USB the same battery may measure (PPG) */
    sim_set_usb(true);
    scen_run_ms(200);
    CHECK_EQ(scen_screen(), UI_SCREEN_HOME); /* USB plugged: Home */
    scen_tap(2);
    scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_PPG_CARD);
    scen_tap(1);
    scen_run_ms(2000);
    CHECK(acq_active());
    scen_tap(1);
    sim_set_usb(false);
    scen_run_ms(300);

    /* battery drops during a measurement: stopped, warning, summary/card */
    set_batt(3900);
    scen_run_ms(11000);
    scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG_CARD);
    scen_tap(1);
    scen_run_ms(500);
    sim_set_thumbs(true);
    scen_run_ms(8000);
    CHECK(acq_active());
    set_batt(3480);
    scen_run_ms(11000);
    CHECK(!acq_active());
    CHECK(scen_analog_all_off());
    CHECK(scen_m()->overlay == UI_OVERLAY_LOW_BATTERY || scen_screen() == UI_SCREEN_SUMMARY);
    scen_run_ms(4000);
    CHECK(scen_screen() == UI_SCREEN_SUMMARY || scen_screen() == UI_SCREEN_ECG_CARD);
    sim_set_thumbs(false);
}

TEST(critical_battery)
{
    scen_boot(false);
    scen_run_ms(300);
    /* no cut-off on USB power */
    sim_set_usb(true);
    set_batt(3250);
    scen_run_ms(25000);
    CHECK(awake());
    CHECK_EQ(scen_m()->overlay, UI_OVERLAY_NONE);
    sim_set_usb(false);
    scen_run_ms(500);
    scen_run_ms(10500);
    CHECK(scen_m()->overlay == UI_OVERLAY_BATT_CRITICAL || in_standby());
    if (!in_standby())
        scen_shot("batt_critical");
    CHECK(scen_run_cond(in_standby, 5000));
    CHECK(all_rails_off());
    /* a button wake with the battery still empty goes straight back */
    scen_tap(2);
    scen_run_ms(500);
    CHECK_EQ(scen_m()->overlay, UI_OVERLAY_BATT_CRITICAL);
    CHECK(scen_run_cond(in_standby, 5000));
    /* CW_ALRT (gauge alert at ATHD) is read and cleared */
    CHECK(!sim_cw_alert_flag());
}

/* Review finding 5: right after the gauge is woken VCELL / SOC read 0; the
 * app took that as an empty cell and went to STANDBY at once. A cold boot
 * with a healthy battery must never show the critical overlay, whatever the
 * gauge's start-up time. */
static bool crit_seen;
static void watch_crit(void)
{
    if (scen_m()->overlay == UI_OVERLAY_BATT_CRITICAL || in_standby())
        crit_seen = true;
}

TEST(cold_boot_gauge_not_ready)
{
    static const unsigned zero_ms[] = {0, 200, 1500, 10000};
    for (size_t i = 0; i < sizeof zero_ms / sizeof zero_ms[0]; i++) {
        sim_reset();
        sim_set_violation_mode(SIM_VIOL_ABORT);
        sim_cw_set_wake_zero_ms(zero_ms[i]);
        sim_set_battery_mv(3900);
        memset(&scen, 0, sizeof scen);
        app_init();
        crit_seen = false;
        CHECK(app_debug()->pwr == APP_PWR_AWAKE);
        bool valid_seen = false;
        uint64_t valid_at = 0;
        for (int k = 0; k < 150; k++) { /* 15 s in 100 ms steps */
            scen_run_ms(100);
            watch_crit();
            if (!valid_seen && scen_m()->batt_valid) {
                valid_seen = true;
                valid_at = sim_now_ms();
            }
        }
        printf("  gauge ready after %5u ms: battery valid at %llu ms, %u mV %u %%\n", zero_ms[i],
               (unsigned long long)valid_at, app_debug()->batt_mv, app_debug()->batt_pct);
        CHECK(!crit_seen);
        CHECK(awake());
        CHECK(valid_seen);
        CHECK(app_debug()->batt_mv > 3800);
        if (zero_ms[i] <= 1500)
            CHECK(valid_at <= zero_ms[i] + 400); /* retried, not one poll later */
        else
            CHECK(valid_at >= zero_ms[i] && valid_at <= zero_ms[i] + 10500); /* next poll */
        CHECK_EQ(sim_facts()->standby_entries, 0);
    }
}

/* One critical reading alone does not shut down: it is confirmed 1 s later. */
static void batt_dip(void *ctx) { (void)ctx; sim_set_battery_mv(3250); }
static void batt_back(void *ctx) { (void)ctx; sim_set_battery_mv(3900); }

TEST(critical_needs_two_readings)
{
    scen_boot(false);
    scen_run_ms(1000);
    CHECK(scen_m()->batt_valid);
    /* polls run every 10 s from boot: dip across the one at ~10 s only */
    sim_at(9800, batt_dip, NULL);
    sim_at(10400, batt_back, NULL);
    uint16_t min_mv = 9999;
    while (sim_now_ms() < 14000) {
        scen_run_ms(50);
        if (app_debug()->batt_mv < min_mv) min_mv = app_debug()->batt_mv;
        CHECK(scen_m()->overlay != UI_OVERLAY_BATT_CRITICAL);
    }
    CHECK(min_mv < BATT_CUTOFF_MV); /* the critical value was read ... */
    CHECK(awake());                 /* ... but not confirmed */
    /* a real drop: confirmed by the second reading, ~1 s later */
    sim_set_battery_mv(3250);
    uint64_t t = sim_now_ms();
    CHECK(scen_run_cond(in_standby, 15000));
    uint64_t dt = sim_now_ms() - t;
    printf("  critical: standby %llu ms after the drop\n", (unsigned long long)dt);
    CHECK(dt <= 10000 + 1000 + 3000 + 500); /* poll + confirm + overlay */
    CHECK(all_rails_off());
}

/* Review finding 10c: a button stuck down after boot refreshed the
 * inactivity timer forever (display on, never STANDBY). It now counts as
 * stuck after BTN_HELD_MAX_MS; standby with it still held must not
 * wake-loop (a held button gives no edges). */
TEST(button_stuck_after_boot)
{
    for (int b = 2; b >= 1; b--) {
        scen_boot(false);
        scen_run_ms(1000);
        uint64_t t_press = sim_now_ms();
        sim_set_button(b, true); /* and never released */
        if (b == 2) {
            /* BUTTON2: its long press (back) fires, then it is ignored */
            CHECK(scen_run_cond(display_off, BTN_HELD_MAX_MS + OVERLAY_FAULT_MS_T +
                                                 UI_DISPLAY_TIMEOUT_S * 1000 + 2000));
            uint64_t dt = sim_now_ms() - t_press;
            printf("  button 2 stuck: display off after %llu ms\n", (unsigned long long)dt);
            CHECK(dt >= UI_DISPLAY_TIMEOUT_S * 1000);
            CHECK(scen_run_cond(in_standby, UI_STANDBY_TIMEOUT_S * 1000 + 2000));
        } else {
            /* BUTTON1: its long press is power-off, which waits for the
             * release; the stuck detection ends the wait */
            CHECK(scen_run_cond(in_standby, BTN_HELD_MAX_MS + 2000));
            printf("  button 1 stuck: standby after %llu ms\n",
                   (unsigned long long)(sim_now_ms() - t_press));
        }
        CHECK(all_rails_off());
        uint32_t entries = sim_facts()->standby_entries;
        uint64_t steps0 = scen.steps;
        scen_run_ms(30ull * 60 * 1000); /* 30 min, button still held */
        CHECK(in_standby());
        CHECK_EQ(app_debug()->wakes, 0);
        CHECK(sim_facts()->standby_entries - entries <= 4); /* timed battery wakes only */
        CHECK(scen.steps - steps0 < 20);
        /* the release is an edge: it wakes */
        sim_set_button(b, false);
        scen_run_ms(500);
        CHECK(awake());
        CHECK_EQ(sim_facts()->wdt_resets, 0);
    }
}

/* Review finding 10d: an event bit nobody handles in STANDBY (RADIO_IRQ in
 * this build) stayed latched, and hal_standby() returns at once while one is
 * pending: the standby loop spun without time passing. */
TEST(standby_unhandled_event_no_spin)
{
    scen_boot(false);
    scen_run_ms(300);
    scen_press(1, 2200);
    scen_run_ms(200);
    CHECK(in_standby());
    uint32_t entries = sim_facts()->standby_entries;
    sim_at(sim_now_ms() + 1000, radio_irq_pulse, NULL);
    uint64_t t_end = sim_now_ms() + 60000;
    sim_clear_stop();
    sim_stop_at(t_end);
    uint64_t steps = 0;
    while (!sim_should_stop() && steps < 1000) {
        app_step();
        steps++;
    }
    sim_clear_stop();
    CHECK(steps < 1000); /* did not spin */
    CHECK(sim_now_ms() >= t_end);
    CHECK(in_standby());
    CHECK(sim_facts()->standby_entries - entries <= 3);
    CHECK_EQ(app_debug()->wakes, 0);
}

int main(void)
{
    RUN(timeouts_and_wake);
    RUN(power_off_long_press);
    RUN(mcp4728_reference_off);
    RUN(periodic_battery_wake);
    RUN(low_battery);
    RUN(critical_battery);
    RUN(cold_boot_gauge_not_ready);
    RUN(critical_needs_two_readings);
    RUN(button_stuck_after_boot);
    RUN(standby_unhandled_event_no_spin);
    return TEST_RESULT();
}
