/* Fault handling: VMID self-check, missing I2C devices, stuck bus, stuck
 * button. Watchdog and loop-latency bounds are checked in every test. */
#include <string.h>

#include "acq.h"
#include "scen.h"

static bool is_running(void) { return scen_live_state() == UI_MEAS_RUNNING; }

static void common_checks(void)
{
    CHECK_EQ(sim_facts()->wdt_resets, 0);
    CHECK_EQ(sim_violation_count(), 0);
    CHECK(scen.max_step_acq_us <= 20000);
    CHECK_EQ(scen.body_en_outside_session, 0);
    CHECK_EQ(scen.measure_req_on_usb, 0);
    CHECK_EQ(scen.vref_outside_ppg, 0);
    CHECK_EQ(sim_facts()->mcp_vref_int_standby, 0);
}

TEST(vmid_fault)
{
    scen_boot(false);
    sim_ecg_set_vmid_error_mv(60);
    scen_run_ms(300);
    scen_tap(2);
    scen_tap(1);
    scen_run_ms(800);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG);
    CHECK_EQ(scen_live_state(), UI_MEAS_FAULT);
    CHECK(scen_m()->fault_text && strstr(scen_m()->fault_text, "VMID 171") != NULL);
    CHECK(scen_analog_all_off());
    CHECK(!sim_rail(SIM_RAIL_BODY_EN));
    scen_shot("vmid_fault");
    scen_tap(1);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG_CARD);
    /* PPG does not depend on VMID */
    scen_tap(2);
    sim_set_finger(true);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    scen_tap(1);
    CHECK(scen_run_cond(is_running, 6000));
    scen_tap(1);
    sim_set_finger(false);
    common_checks();
}

TEST(mcp_absent)
{
    scen_boot(false);
    sim_i2c_set_present(I2C_ADDR_MCP4728, false);
    scen_reboot();
    scen_run_ms(300);
    CHECK(!app_debug()->mcp_ok);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    scen_tap(2);
    scen_tap(2);
    scen_tap(1); /* PPG */
    scen_run_ms(300);
    CHECK_EQ(scen_live_state(), UI_MEAS_FAULT);
    CHECK(scen_analog_all_off());
    scen_shot("mcp_absent_ppg");
    scen_tap(1);
    scen_tap(2);
    scen_tap(1); /* BOTH */
    scen_run_ms(300);
    CHECK_EQ(scen_screen(), UI_SCREEN_BOTH);
    CHECK_EQ(scen_live_state(), UI_MEAS_FAULT);
    CHECK(scen_analog_all_off());
    scen_press(2, 2100);
    /* ECG still works */
    scen_tap(2);
    scen_tap(1);
    scen_run_ms(500);
    sim_set_thumbs(true);
    CHECK(scen_run_cond(is_running, 6000));
    scen_run_ms(10000);
    CHECK_NEAR(scen_m()->live.hr_bpm, tc.hr_bpm, 2);
    scen_tap(1);
    CHECK(scen_analog_all_off());
    sim_set_thumbs(false);
    common_checks();
}

TEST(gauge_absent)
{
    scen_boot(false);
    sim_i2c_set_present(I2C_ADDR_CW2015, false);
    sim_set_battery_mv(3250); /* would be critical; unknown -> no cut-off */
    scen_reboot();
    scen_run_ms(30000);
    CHECK(!scen_m()->batt_valid);
    CHECK_EQ(app_debug()->pwr, APP_PWR_AWAKE);
    CHECK_EQ(scen_m()->overlay, UI_OVERLAY_NONE);
    CHECK_EQ(scen_screen(), UI_SCREEN_HOME);
    scen_shot("gauge_absent_home");
    /* measuring still works */
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    scen_tap(2);
    scen_tap(1);
    scen_run_ms(500);
    sim_set_thumbs(true);
    CHECK(scen_run_cond(is_running, 6000));
    scen_tap(1);
    sim_set_thumbs(false);
    /* gauge comes back: picked up at the next poll */
    sim_i2c_set_present(I2C_ADDR_CW2015, true);
    sim_set_battery_mv(3900);
    scen_run_ms(11000);
    CHECK(scen_m()->batt_valid);
    common_checks();
}

TEST(stuck_bus)
{
    scen_boot(false);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    scen_run_ms(300);
    for (int i = 0; i < 3; i++)
        scen_tap(2);
    sim_set_thumbs(true);
    sim_set_finger(true);
    scen_tap(1); /* BOTH: LED writes and gauge reads during acquisition */
    CHECK(scen_run_cond(is_running, 6000));
    uint32_t rec0 = sim_facts()->i2c_recoveries;
    for (int k = 0; k < 4; k++) {
        sim_i2c_stick_bus(true);
        scen_run_ms(10500); /* at least one gauge poll */
    }
    CHECK(sim_facts()->i2c_recoveries >= rec0 + 4);
    CHECK(!sim_i2c_bus_stuck());
    CHECK(scen_m()->batt_valid);
    CHECK(is_running());
    CHECK_NEAR(scen_m()->live.hr_bpm, tc.hr_bpm, 2);
    printf("  max step with a stuck bus %llu us\n", (unsigned long long)scen.max_step_acq_us);
    CHECK_EQ(hal_ecg_overruns(), 0);
    scen_tap(1);
    CHECK(scen_analog_all_off());
    sim_set_thumbs(false);
    sim_set_finger(false);
    common_checks();
}

TEST(stuck_button)
{
    sim_reset();
    sim_set_violation_mode(SIM_VIOL_ABORT);
    memset(&scen, 0, sizeof scen);
    sim_set_button(1, true); /* held from power-up */
    app_init();
    scen_run_ms(4000);
    CHECK_EQ(scen_m()->overlay, UI_OVERLAY_NONE); /* not yet, and no power-off */
    scen_run_ms(1500);
    CHECK_EQ(scen_m()->overlay, UI_OVERLAY_FAULT);
    CHECK(scen_m()->fault_text && strstr(scen_m()->fault_text, "Button 1") != NULL);
    scen_shot("stuck_button");
    scen_run_ms(3500);
    CHECK_EQ(scen_m()->overlay, UI_OVERLAY_NONE);
    scen_run_ms(10000);
    CHECK_EQ(scen_m()->overlay, UI_OVERLAY_NONE); /* once */
    CHECK_EQ(app_debug()->pwr, APP_PWR_AWAKE);
    /* the other button still works */
    scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG_CARD);
    common_checks();
}

int main(void)
{
    RUN(vmid_fault);
    RUN(mcp_absent);
    RUN(gauge_absent);
    RUN(stuck_bus);
    RUN(stuck_button);
    return TEST_RESULT();
}
