/* Power policy: display / standby timeouts, wake, power-off, battery. */
#include <string.h>

#include "acq.h"
#include "scen.h"

static bool in_standby(void) { return app_debug()->pwr == APP_PWR_STANDBY; }
static bool display_off(void) { return app_debug()->pwr == APP_PWR_DISPLAY_OFF; }
static bool awake(void) { return app_debug()->pwr == APP_PWR_AWAKE; }

static bool all_rails_off(void)
{
    return scen_analog_all_off() && !sim_pin(PIN_OLED_EN) && !sim_oled_bus_attached() &&
           !sim_pin(PIN_OLED_DC) && !sim_pin(PIN_OLED_CS) && !sim_pin(PIN_OLED_RESET);
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
    /* the release edge did not wake it again */
    scen_run_ms(20000);
    CHECK(in_standby());
    CHECK_EQ(app_debug()->wakes, 0);
    sim_set_finger(false);
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

int main(void)
{
    RUN(timeouts_and_wake);
    RUN(power_off_long_press);
    RUN(periodic_battery_wake);
    RUN(low_battery);
    RUN(critical_battery);
    return TEST_RESULT();
}
