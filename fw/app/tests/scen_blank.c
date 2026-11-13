/* The application built with APP_PPG_BLANK_DISPLAY: the panel is switched
 * off (AEh, charge pump left on) while PPG is RUNNING, and back on after. */
#include "acq.h"
#include "scen.h"

static bool is_running(void) { return acq_status()->settled; } /* no frames while blanked */
static bool is_summary(void) { return scen_screen() == UI_SCREEN_SUMMARY; }

TEST(blank_during_ppg)
{
    scen_boot(false);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    scen_run_ms(300);
    scen_tap(2);
    scen_tap(2);
    scen_tap(1);
    scen_run_ms(500);
    CHECK(sim_oled_info().display_on);
    sim_set_finger(true);
    CHECK(scen_run_cond(is_running, 6000));
    scen_run_ms(500);
    sim_oled_info_t o = sim_oled_info();
    CHECK(o.powered && !o.display_on && o.charge_pump);
    CHECK_EQ(scen_lit_pixels(), 0);
    uint32_t data0 = o.data_bytes;
    scen_run_ms(15000);
    CHECK_EQ(sim_oled_info().data_bytes, data0); /* no SPI traffic over the TIA */
    CHECK_NEAR(acq_status()->pulse_bpm, tc.hr_bpm, 2);
    sim_set_finger(false);
    CHECK(scen_run_cond(is_summary, 4000));
    scen_run_ms(300);
    CHECK(sim_oled_info().display_on);
    CHECK(scen_lit_pixels() > 100);
    CHECK(scen_analog_all_off());
}

int main(void)
{
    RUN(blank_during_ppg);
    return TEST_RESULT();
}
