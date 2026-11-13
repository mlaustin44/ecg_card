/* USB power and the ECG interlock. */
#include "acq.h"
#include "scen.h"

static bool is_running(void) { return scen_live_state() == UI_MEAS_RUNNING; }
static bool is_summary(void) { return scen_screen() == UI_SCREEN_SUMMARY; }

static bool ecg_pins_low(void)
{
    return !sim_pin(PIN_MEASURE_REQ) && !sim_pin(PIN_FAST_RESTORE) && !sim_pin(PIN_GAIN_HI) &&
           !sim_pin(PIN_RLD_L_EN) && !sim_pin(PIN_RLD_R_EN) && !sim_pin(PIN_ANA_EN) &&
           !sim_pin(PIN_BUCK_PS_SYNC);
}

TEST(start_refused_on_usb)
{
    scen_boot(false);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    sim_set_usb(true);
    scen_run_ms(300);
    CHECK(scen_m()->usb_present && scen_m()->charging);
    scen_shot("home_charging");
    scen_tap(2);
    scen_run_ms(100);
    scen_shot("ecg_card_usb");
    sim_set_thumbs(true);
    scen_tap(1);
    scen_run_ms(1500);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG);
    CHECK_EQ(scen_live_state(), UI_MEAS_USB_BLOCKED);
    CHECK(ecg_pins_low());
    CHECK(!sim_rail(SIM_RAIL_BODY_EN));
    scen_shot("ecg_usb_blocked");
    /* BOTH is blocked the same way */
    scen_tap(1);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG_CARD);
    scen_tap(2);
    scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_BOTH_CARD);
    scen_tap(1);
    scen_run_ms(1000);
    CHECK_EQ(scen_live_state(), UI_MEAS_USB_BLOCKED);
    CHECK(ecg_pins_low() && !sim_pin(PIN_VLED_EN));
    /* unplug while on the blocked screen: the measurement arms */
    sim_set_usb(false);
    scen_run_ms(800);
    CHECK_EQ(scen_screen(), UI_SCREEN_BOTH);
    CHECK(sim_rail(SIM_RAIL_BODY_EN));
    CHECK(scen_run_cond(is_running, 6000));
    scen_tap(1);
    CHECK(scen_analog_all_off());
    CHECK_EQ(scen.measure_req_on_usb, 0);
    CHECK_EQ(scen.body_en_outside_session, 0);
    sim_set_thumbs(false);
}

TEST(usb_mid_record)
{
    scen_boot(false);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    scen_run_ms(300);
    scen_tap(2);
    scen_tap(1);
    scen_run_ms(500);
    sim_set_thumbs(true);
    CHECK(scen_run_cond(is_running, 6000));
    scen_run_ms(15000);
    CHECK(sim_rail(SIM_RAIL_BODY_EN));
    sim_set_usb(true);
    /* the HAL ISR dropped MEASURE_REQ; the app ends the record within a pass */
    CHECK(!sim_pin(PIN_MEASURE_REQ));
    scen_run_ms(2);
    CHECK(ecg_pins_low());
    CHECK(scen_analog_all_off());
    scen_run_ms(200);
    CHECK_EQ(scen_m()->overlay, UI_OVERLAY_USB_STOPPED);
    scen_shot("usb_stopped_overlay");
    CHECK(scen_run_cond(is_summary, 3000));
    scen_run_ms(100);
    CHECK(scen_m()->summary.stopped_by_usb);
    CHECK(scen_m()->summary.valid);
    CHECK_NEAR(scen_m()->summary.hr_avg, tc.hr_bpm, 2);
    scen_shot("summary_usb");
    /* impossible to start ECG until unplugged */
    scen_tap(1);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG_CARD);
    scen_tap(1);
    scen_run_ms(1000);
    CHECK_EQ(scen_live_state(), UI_MEAS_USB_BLOCKED);
    CHECK(ecg_pins_low());
    scen_tap(1);
    sim_set_usb(false);
    scen_run_ms(300);
    CHECK_EQ(scen_screen(), UI_SCREEN_HOME); /* unplug shows Home */
    CHECK_EQ(scen.measure_req_on_usb, 0);
    CHECK_EQ(scen.body_en_outside_session, 0);
    sim_set_thumbs(false);
}

TEST(usb_while_waiting)
{
    scen_boot(false);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    scen_run_ms(300);
    scen_tap(2);
    scen_tap(1);
    scen_run_ms(1000);
    CHECK(sim_pin(PIN_MEASURE_REQ));
    sim_set_usb(true);
    scen_run_ms(2);
    CHECK(scen_analog_all_off());
    scen_run_ms(3000);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG);
    CHECK_EQ(scen_live_state(), UI_MEAS_USB_BLOCKED);
    /* plug during the VMID wait of the start sequence */
    sim_set_usb(false);
    scen_run_ms(100);
    CHECK(sim_pin(PIN_ANA_EN) && !sim_pin(PIN_MEASURE_REQ));
    sim_set_usb(true);
    scen_run_ms(1000);
    CHECK(scen_analog_all_off());
    CHECK_EQ(scen_live_state(), UI_MEAS_USB_BLOCKED);
    CHECK_EQ(scen.measure_req_on_usb, 0);
}

int main(void)
{
    RUN(start_refused_on_usb);
    RUN(usb_mid_record);
    RUN(usb_while_waiting);
    return TEST_RESULT();
}
