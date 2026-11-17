/* Boot (and its designer credit screen), carousel, settings persistence,
 * corrupted NV, diagnostics. */
#include <string.h>

#include "gfx.h"
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

static bool on_home(void) { return scen_screen() == UI_SCREEN_HOME; }

/* Reset: the boot screen with the designer's name for ~2 s, everything
 * starting underneath it, then Home. */
TEST(boot_screen)
{
    scen_boot_splash(false);
    scen_run_ms(30);
    CHECK_EQ(scen_screen(), UI_SCREEN_BOOT);
    CHECK(scen.name_runs > 0);
    CHECK_EQ(scen.name_h, FONTMED_H);
    CHECK_EQ(scen_m()->overlay, UI_OVERLAY_NONE);
    /* the boot work is not held up: gauge read, panel up, radio advertising */
    scen_run_ms(1170);
    CHECK_EQ(scen_screen(), UI_SCREEN_BOOT);
    CHECK(scen_m()->screen_ms >= 1100 && scen_m()->screen_ms <= 1200);
    CHECK(scen_m()->batt_valid);
    CHECK(sim_oled_info().init_complete);
    CHECK_EQ(scen_m()->ble_state, UI_BLE_ADVERTISING);
    CHECK_EQ(sim_facts()->mcp_eeprom_writes, 4);
    CHECK(scen_analog_all_off());
    CHECK(scen_lit_pixels() > 600);
    scen_shot("boot_splash");
    uint32_t boot_frames = scen.boot_frames;
    CHECK(boot_frames >= 25); /* the trace animates (25 fps) */
    CHECK(scen_run_cond(on_home, 1500));
    uint64_t t_home = sim_now_ms();
    printf("  boot screen until %llu ms\n", (unsigned long long)t_home);
    CHECK(t_home >= 2000 && t_home <= 2100);
    CHECK(scen_lit_pixels() > 200);
    /* and the display timeout counts from Home */
    scen_run_until(t_home + UI_DISPLAY_TIMEOUT_S * 1000 - 200);
    CHECK_EQ(app_debug()->pwr, APP_PWR_AWAKE);
    scen_run_ms(400);
    CHECK_EQ(app_debug()->pwr, APP_PWR_DISPLAY_OFF);
    CHECK_EQ(scen.power_down_frames, 0);
    CHECK_EQ(sim_violation_count(), 0);
    /* a warm reset shows it again */
    uint32_t names = scen.name_runs;
    scen_reboot_splash();
    scen_run_ms(500);
    CHECK_EQ(scen_screen(), UI_SCREEN_BOOT);
    CHECK(scen.name_runs > names);
    CHECK(scen_run_cond(on_home, 2000));
}

/* A press skips it to Home and does nothing else (no navigation, no event,
 * no power-off when held); the next press navigates as usual. */
TEST(boot_screen_skip)
{
    scen_boot_splash(false);
    scen_run_ms(500);
    CHECK_EQ(scen_screen(), UI_SCREEN_BOOT);
    uint64_t t = sim_now_ms() + 1;
    sim_at_button(t, 2, true);
    sim_at_button(t + 100, 2, false);
    scen_run_until(t + 40);
    CHECK_EQ(scen_screen(), UI_SCREEN_HOME); /* on the press, not the release */
    scen_run_until(t + 300);
    CHECK_EQ(scen_screen(), UI_SCREEN_HOME); /* not the ECG card */
    scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG_CARD);
    /* BUTTON1 held through: skips, and is no power-off */
    scen_reboot_splash();
    scen_run_ms(300);
    CHECK_EQ(scen_screen(), UI_SCREEN_BOOT);
    scen_press(1, 3000);
    scen_run_ms(2500);
    CHECK_EQ(app_debug()->pwr, APP_PWR_AWAKE);
    CHECK_EQ(scen_screen(), UI_SCREEN_HOME);
    CHECK_EQ(scen_m()->overlay, UI_OVERLAY_NONE);
    CHECK_EQ(scen.power_down_frames, 0);
    CHECK_EQ(app_debug()->sessions, 0);
    /* BUTTON1 short on the boot screen: no measurement either */
    scen_reboot_splash();
    scen_run_ms(300);
    uint32_t ev0 = scen_decode_stream().buttons;
    scen_tap(1);
    CHECK_EQ(scen_screen(), UI_SCREEN_HOME);
    CHECK_EQ(scen_decode_stream().buttons, ev0); /* no button event on the stream */
    scen_tap(2);
    CHECK_EQ(scen_decode_stream().buttons, ev0 + 1);
    scen_tap(1); /* the ECG card: this one starts */
    scen_run_ms(200);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG);
    CHECK_EQ(app_debug()->sessions, 1);
    scen_tap(1);
    scen_run_ms(200);
    CHECK(scen_analog_all_off());
    CHECK_EQ(sim_violation_count(), 0);
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
    /* Settings, closed: BUTTON2 moves on, BUTTON1 opens; open, BUTTON2 goes
     * round the items and "Exit settings", BUTTON1 there closes it */
    CHECK_EQ(scen_m()->settings_cursor, UI_SET_CLOSED);
    scen_tap(1);
    CHECK_EQ(scen_m()->settings_cursor, 0);
    scen_shot("settings_open");
    for (int i = 0; i < SET_ITEM_COUNT; i++) {
        scen_tap(2);
        CHECK_EQ(scen_screen(), UI_SCREEN_SETTINGS);
    }
    CHECK_EQ(scen_m()->settings_cursor, UI_SET_EXIT);
    scen_shot("settings_exit");
    scen_tap(2); /* round to the first item */
    CHECK_EQ(scen_m()->settings_cursor, 0);
    for (int i = 0; i < SET_ITEM_COUNT; i++)
        scen_tap(2);
    const settings_t before = *app_settings();
    scen_tap(1); /* Exit: closed, nothing changed */
    CHECK_EQ(scen_screen(), UI_SCREEN_SETTINGS);
    CHECK_EQ(scen_m()->settings_cursor, UI_SET_CLOSED);
    CHECK(memcmp(&before, app_settings(), sizeof before) == 0);
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
    scen_tap(1); /* open */
    CHECK_EQ(scen_m()->settings_cursor, 0);
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
    settings_t s = {GAIN_HI, NOTCH_50, RLD_OFF, LED_LIMIT_10MA, SWEEP_6S, BLE_ON, RESTORE_FAST, VIEW_TRACE};
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
    for (int i = 0; i < 5; i++) /* past Settings (closed) */
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
    RUN(boot_screen);
    RUN(boot_screen_skip);
    RUN(boot_home);
    RUN(carousel);
    RUN(settings_persist);
    RUN(corrupt_nv);
    RUN(diagnostics);
    printf("max step %llu us\n", (unsigned long long)scen.max_step_us);
    return TEST_RESULT();
}
