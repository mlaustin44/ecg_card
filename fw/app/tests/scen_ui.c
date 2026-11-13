/* Boot, carousel, settings persistence, corrupted NV, diagnostics. */
#include <string.h>

#include "scen.h"
#include "settings_store.h"
#include "ssd1306.h"

TEST(boot_home)
{
    scen_boot(false);
    CHECK(sim_facts()->mcp_eeprom_writes == 4); /* factory EEPROM fixed once */
    CHECK_EQ(sim_mcp_pd(0), 1);                   /* LED DAC powered down at boot */
    scen_run_ms(400);
    sim_oled_info_t o = sim_oled_info();
    CHECK(o.powered && o.init_complete && o.image_settled);
    CHECK(o.reset_pulse_seen);
    CHECK_EQ(o.early_reset_releases, 0);
    CHECK(scen_screen() == UI_SCREEN_HOME);
    CHECK(scen_m()->batt_valid);
    CHECK(scen_lit_pixels() > 200);
    CHECK(scen_analog_all_off());
    scen_shot("boot_home");
    /* the very first I2C transactions went to the MCP4728 */
    const sim_i2c_txn_t *t = sim_i2c_log_get(0);
    CHECK(t && t->addr == I2C_ADDR_MCP4728);
    CHECK_EQ(sim_oled_info().dirty_power_offs, 0);
    /* a second boot (warm reset: the panel loses power abruptly, which the
     * model counts as a dirty power-off) does not rewrite the EEPROM */
    scen_reboot();
    scen_run_ms(300);
    CHECK_EQ(sim_facts()->mcp_eeprom_writes, 4);
    CHECK(scen_lit_pixels() > 200);
}

TEST(carousel)
{
    scen_boot(false);
    scen_run_ms(300);
    const ui_screen_t order[] = {UI_SCREEN_ECG_CARD, UI_SCREEN_PPG_CARD, UI_SCREEN_BOTH_CARD,
                                 UI_SCREEN_SETTINGS};
    for (int i = 0; i < 4; i++) {
        scen_tap(2);
        scen_run_ms(100);
        CHECK_EQ(scen_screen(), order[i]);
    }
    scen_shot("settings");
    /* Settings: BUTTON2 walks the items, then moves on to About */
    for (int i = 0; i < SET_ITEM_COUNT - 1; i++) {
        scen_tap(2);
        CHECK_EQ(scen_screen(), UI_SCREEN_SETTINGS);
    }
    CHECK_EQ(scen_m()->settings_cursor, SET_ITEM_COUNT - 1);
    scen_tap(2);
    scen_run_ms(100);
    CHECK_EQ(scen_screen(), UI_SCREEN_ABOUT);
    scen_shot("about");
    scen_tap(2);
    scen_run_ms(100);
    CHECK_EQ(scen_screen(), UI_SCREEN_HOME);
    /* BUTTON2 long: back to Home from anywhere */
    scen_tap(2);
    scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_PPG_CARD);
    scen_press(2, 2200);
    scen_run_ms(100);
    CHECK_EQ(scen_screen(), UI_SCREEN_HOME);
    CHECK(scen_analog_all_off());
    CHECK_EQ(sim_violation_count(), 0);
}

static void goto_settings(void)
{
    for (int i = 0; i < 4; i++)
        scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_SETTINGS);
}

TEST(settings_persist)
{
    scen_boot(false);
    scen_run_ms(300);
    goto_settings();
    scen_tap(1);      /* gain AUTO -> 558x */
    scen_tap(2);
    scen_tap(2);
    scen_tap(1);      /* RLD BOTH -> LEFT */
    scen_tap(2);
    scen_tap(1);      /* LED 15 -> 5 mA */
    scen_run_ms(200);
    CHECK_EQ(app_settings()->gain, GAIN_LO);
    CHECK_EQ(app_settings()->rld, RLD_LEFT);
    CHECK_EQ(app_settings()->led_limit, LED_LIMIT_5MA);
    scen_shot("settings_changed");
    uint32_t w0 = sim_facts()->nv_writes;
    scen_run_ms(4000); /* idle on the page: saved once */
    CHECK_EQ(sim_facts()->nv_writes, w0 + 1);
    scen_run_ms(4000);
    CHECK_EQ(sim_facts()->nv_writes, w0 + 1); /* only on change */
    scen_reboot();
    scen_run_ms(300);
    CHECK_EQ(app_settings()->gain, GAIN_LO);
    CHECK_EQ(app_settings()->rld, RLD_LEFT);
    CHECK_EQ(app_settings()->led_limit, LED_LIMIT_5MA);
    CHECK_EQ(app_debug()->settings_status, SETTINGS_LOADED);
    /* cycling a value all the way round writes nothing */
    goto_settings();
    for (int i = 0; i < GAIN_MODE_COUNT; i++)
        scen_tap(1);
    scen_run_ms(4000);
    CHECK_EQ(sim_facts()->nv_writes, w0 + 1);
}

TEST(corrupt_nv)
{
    scen_boot(false);
    settings_t s = {GAIN_HI, NOTCH_50, RLD_OFF, LED_LIMIT_10MA, SWEEP_6S, BLE_ON};
    uint8_t b[SETTINGS_BLOB_LEN];
    settings_encode(&s, b);
    sim_nv_set(b, sizeof b);
    scen_reboot();
    CHECK_EQ(app_settings()->notch, NOTCH_50);
    sim_nv_corrupt(6, 0x10);
    scen_reboot();
    scen_run_ms(300);
    const settings_t d = SETTINGS_DEFAULTS;
    CHECK(memcmp(app_settings(), &d, sizeof d) == 0);
    CHECK_EQ(app_debug()->settings_status, SETTINGS_CORRUPT);
    CHECK(strstr(sim_log_text(), "corrupt") != NULL);
    CHECK_EQ(scen_screen(), UI_SCREEN_HOME);
}

TEST(diagnostics)
{
    scen_boot(false);
    scen_run_ms(300);
    for (int i = 0; i < 4 + SET_ITEM_COUNT; i++) /* through the Settings items */
        scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_ABOUT);
    scen_press_both(2300);
    scen_run_ms(300);
    CHECK_EQ(scen_screen(), UI_SCREEN_DIAG);
    CHECK(strstr(scen_m()->diag[0], "PINS") != NULL);
    scen_shot("diag1_pins");
    scen_tap(2); /* analog page: ANA_EN on, no MEASURE_REQ */
    scen_run_ms(600);
    CHECK(sim_pin(PIN_ANA_EN));
    CHECK(!sim_pin(PIN_MEASURE_REQ) && !sim_pin(PIN_VLED_EN));
    CHECK(strstr(scen_m()->diag[1], "VMID") != NULL);
    scen_shot("diag2_analog");
    scen_tap(2);
    scen_run_ms(400);
    CHECK(!sim_pin(PIN_ANA_EN));
    CHECK(strstr(scen_m()->diag[0], "60:ACK") != NULL);
    CHECK(strstr(scen_m()->diag[1], "0x73") != NULL);
    scen_shot("diag3_i2c");
    scen_tap(2);
    scen_run_ms(300);
    scen_shot("diag4_acq");
    scen_tap(2);
    scen_run_ms(300);
    CHECK(strstr(scen_m()->diag[0], "FIRMWARE") != NULL);
    scen_shot("diag5_fw");
    scen_tap(2);
    scen_run_ms(300);
    CHECK(strstr(scen_m()->diag[0], "RADIO") != NULL);
    scen_shot("diag6_radio");
    scen_tap(2);
    scen_tap(2); /* analog again, then leave with BUTTON1 */
    scen_run_ms(400);
    CHECK(sim_pin(PIN_ANA_EN));
    scen_tap(1);
    scen_run_ms(100);
    CHECK_EQ(scen_screen(), UI_SCREEN_ABOUT);
    CHECK(scen_analog_all_off());
    CHECK_EQ(scen.analog_outside, 0);
}

int main(void)
{
    RUN(boot_home);
    RUN(carousel);
    RUN(settings_persist);
    RUN(corrupt_nv);
    RUN(diagnostics);
    printf("max step %llu us\n", (unsigned long long)scen.max_step_us);
    return TEST_RESULT();
}
