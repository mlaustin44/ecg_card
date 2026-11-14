/* The radio link against the simulator's fake CH585 (sim.h "radio link"):
 * power-up / HELLO / advertising with the pin order checked by the
 * simulator's invariants (ABORT mode), HELLO faults, streaming to a phone,
 * phone commands and every refusal, timeouts, BLE Off, STANDBY handshake,
 * corrupted input, TX backlog, program mode and the device log over BLE. */
#define _GNU_SOURCE /* memmem */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "acq.h"
#include "radio.h"
#include "scen.h"
#include "settings_store.h"
#include "stream.h"

#define SUB_ALL (LINK_SUB_ECG | LINK_SUB_PPG | LINK_SUB_EVENTS | LINK_SUB_STATUS | LINK_SUB_HR)

static bool is_running(void) { return scen_live_state() == UI_MEAS_RUNNING; }
static bool is_summary(void) { return scen_screen() == UI_SCREEN_SUMMARY; }
static bool advertising(void) { return radio_state() == RADIO_ADVERTISING; }
static bool connected(void) { return radio_state() == RADIO_CONNECTED; }
static bool radio_off(void) { return radio_state() == RADIO_OFF; }
static bool in_standby(void) { return app_debug()->pwr == APP_PWR_STANDBY; }
static bool display_off(void) { return app_debug()->pwr == APP_PWR_DISPLAY_OFF; }
static bool log_has(const char *s) { return strstr(sim_log_text(), s) != NULL; }

static size_t count_frames(uint8_t type, uint8_t op)
{
    size_t n = 0;
    for (size_t i = 0; i < sim_radio_log_count(); i++) {
        const sim_link_frame_t *f = sim_radio_log_get(i);
        if (f->type == type && (op == 0 || (f->len && f->p[0] == op)))
            n++;
    }
    return n;
}

static void end_checks(void)
{
    CHECK_EQ(sim_violation_count(), 0);
    CHECK_EQ(sim_facts()->wdt_resets, 0);
    CHECK_EQ(sim_radio_info()->unclean_offs, 0);
    CHECK_EQ(sim_radio_info()->after_shutdown, 0);
    CHECK_EQ(sim_radio_info()->oversize_notify, 0);
    CHECK_EQ(radio_info()->naks, 0); /* no ADV_START while connected, no bad args */
    CHECK(sim_radio_info()->max_shutdown_wait_us <= LINK_SHUTDOWN_WAIT_MS * 1000u + 1000u);
    CHECK_EQ(scen.body_en_outside_session, 0);
    CHECK_EQ(scen.measure_req_on_usb, 0);
}

static void boot_connected(uint8_t subs)
{
    scen_boot(false);
    CHECK(scen_run_cond(advertising, 1000));
    scen_run_ms(50);
    CHECK(sim_phone_connect(247, 16, subs));
    CHECK(scen_run_cond(connected, 100));
}

/* Send a Control write; run until the MCU's ACK (<= 300 ms). Returns the
 * result, 0xFF if none. Checks the STREAM_EV_CMD_RESULT event when the
 * phone is subscribed to Events. */
static uint8_t phone(const uint8_t *p, size_t n)
{
    uint32_t results0 = sim_radio_info()->events_by_kind[STREAM_EV_CMD_RESULT];
    int seq = sim_phone_command(p, n);
    CHECK(seq >= 0);
    if (seq < 0)
        return 0xFF;
    uint8_t r = 0xFF;
    for (int i = 0; i < 300 && !sim_radio_ack_of((uint8_t)seq, &r); i++)
        scen_run_ms(1);
    scen_run_ms(3);
    if (sim_radio_info()->subs & LINK_SUB_EVENTS) {
        uint8_t ev = 0xFF;
        CHECK_EQ(sim_radio_info()->events_by_kind[STREAM_EV_CMD_RESULT], results0 + 1);
        CHECK(sim_radio_cmd_result(n ? p[0] : 0, &ev));
        CHECK_EQ(ev, r);
    }
    return r;
}
static uint8_t cmd1(uint8_t c) { return phone(&c, 1); }
static uint8_t cmd2(uint8_t c, uint8_t a)
{
    uint8_t p[2] = {c, a};
    return phone(p, 2);
}
static uint8_t cmd_set(uint8_t item, uint8_t v)
{
    uint8_t p[3] = {LINK_CMD_SET, item, v};
    return phone(p, 3);
}

/* ------------------------------------------------------------------------ */

TEST(power_up_hello_advertise)
{
    scen_boot(false);
    CHECK(scen_run_cond(advertising, 1000));
    scen_run_ms(100);
    /* pins: RADIO_EN first, then UART0 (the invariants abort otherwise) */
    CHECK(sim_radio_trace_count() >= 2);
    CHECK_EQ(sim_radio_trace_get(0, NULL), SIM_RT_RADIO_EN_HIGH);
    CHECK_EQ(sim_radio_trace_get(1, NULL), SIM_RT_UART_ON);
    CHECK_EQ(sim_radio_info()->power_ups, 1);
    /* READY: SET_INFO, STATUS, ADV_START, in this order */
    CHECK(sim_radio_log_count() >= 3);
    const sim_link_frame_t *f0 = sim_radio_log_get(0), *f1 = sim_radio_log_get(1),
                           *f2 = sim_radio_log_get(2);
    CHECK(f0->type == LINK_RADIO_CTRL && f0->p[0] == LINK_RC_SET_INFO);
    CHECK(f1->type == LINK_STATUS && f1->len == LINK_STATUS_LEN);
    CHECK(f2->type == LINK_RADIO_CTRL && f2->p[0] == LINK_RC_ADV_START);
    CHECK(strcmp(sim_radio_info()->mcu_info, APP_FW_VERSION) == 0);
    CHECK_EQ(count_frames(LINK_RADIO_CTRL, LINK_RC_ADV_START), 1);
    CHECK_EQ(count_frames(LINK_RADIO_CTRL, LINK_RC_SET_INFO), 1);
    CHECK_EQ(sim_radio_info()->conn_state, LINK_CS_ADV_FAST);
    const uint8_t *st = sim_radio_info()->last_status;
    CHECK_EQ(st[0], LINK_PROTO_VERSION);
    CHECK_EQ(st[1], 0);     /* idle */
    CHECK_EQ(st[2], 0xFF);  /* no measurement */
    CHECK_EQ(st[14], APP_FW_MAJOR);
    CHECK_EQ(st[15], APP_FW_MINOR);
    CHECK(log_has("ble: HELLO fw 1.2.3 proto 1 addr A1:B2:C3:D4:E5:F6"));
    CHECK(log_has("radio: CH585 link up"));
    CHECK_EQ(scen_m()->ble_state, UI_BLE_ADVERTISING);
    scen_shot("radio_home_adv");
    /* About: radio firmware and the device name */
    for (int i = 0; i < 4 + SET_ITEM_COUNT; i++)
        scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_ABOUT);
    CHECK(scen_m()->radio_fw && strcmp(scen_m()->radio_fw, "1.2.3") == 0);
    CHECK(scen_m()->ble_id && strcmp(scen_m()->ble_id, "E5F6") == 0);
    scen_shot("radio_about");
    /* LINK_STATUS every 5 s at least while advertising */
    uint32_t s0 = sim_radio_info()->status_frames;
    scen_run_ms(10500);
    CHECK(sim_radio_info()->status_frames - s0 >= 2);
    end_checks();
}

TEST(hello_missing_retry_fault)
{
    /* one silent boot: the retry succeeds */
    sim_reset();
    sim_set_violation_mode(SIM_VIOL_ABORT);
    sim_radio_cfg_t c = sim_radio_defaults();
    c.silent_boots = 1;
    sim_radio_config(&c);
    memset(&scen, 0, sizeof scen);
    app_init();
    CHECK(scen_run_cond(advertising, 2000));
    CHECK_EQ(sim_radio_info()->power_ups, 2);
    CHECK(log_has("no HELLO: power-cycling"));

    /* never a HELLO: two tries, then FAULT; the rest of the card works */
    sim_reset();
    sim_set_violation_mode(SIM_VIOL_ABORT);
    c.silent_boots = UINT32_MAX;
    sim_radio_config(&c);
    memset(&scen, 0, sizeof scen);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    app_init();
    scen_skip_boot_screen();
    scen_run_ms(2000);
    CHECK_EQ(radio_state(), RADIO_FAULT);
    CHECK_EQ(sim_radio_info()->power_ups, 2);
    CHECK(scen_radio_off());
    /* RADIO_EN up, UART up, UART down, RADIO_EN down, twice */
    CHECK_EQ(sim_radio_trace_count(), 8);
    for (size_t i = 0; i < 8; i += 4) {
        uint64_t t_on, t_off;
        CHECK_EQ(sim_radio_trace_get(i, &t_on), SIM_RT_RADIO_EN_HIGH);
        CHECK_EQ(sim_radio_trace_get(i + 1, NULL), SIM_RT_UART_ON);
        CHECK_EQ(sim_radio_trace_get(i + 2, NULL), SIM_RT_UART_OFF);
        CHECK_EQ(sim_radio_trace_get(i + 3, &t_off), SIM_RT_RADIO_EN_LOW);
        printf("  try %zu: powered %llu us\n", i / 4 + 1, (unsigned long long)(t_off - t_on));
        CHECK(t_off - t_on >= LINK_HELLO_TIMEOUT_MS * 1000u && t_off - t_on <= LINK_HELLO_TIMEOUT_MS * 1000u + 3000u);
    }
    CHECK(log_has("ble: radio FAULT: no HELLO after 2 power-ups"));
    CHECK_EQ(scen_m()->ble_state, UI_BLE_FAULT);
    scen_shot("radio_fault_home");
    scen_tap(2);
    scen_shot("radio_fault_card");
    scen_tap(1);
    scen_run_ms(500);
    sim_set_thumbs(true);
    CHECK(scen_run_cond(is_running, 6000));
    scen_run_ms(12000);
    CHECK_NEAR(scen_m()->live.hr_bpm, tc.hr_bpm, 2);
    sim_set_thumbs(false);
    CHECK(scen_run_cond(is_summary, 5000));
    CHECK_EQ(radio_state(), RADIO_FAULT); /* buttons do not retry a fault */
    CHECK_EQ(sim_radio_info()->power_ups, 2);
    /* standby and a wake: one more try (two power-ups) */
    scen_power_off();
    scen_run_ms(300);
    CHECK(in_standby());
    scen_tap(2);
    scen_run_ms(1500);
    CHECK_EQ(sim_radio_info()->power_ups, 4);
    CHECK_EQ(radio_state(), RADIO_FAULT);
    CHECK(scen.max_step_acq_us <= 20000);
    end_checks();
}

/* BOTH session started with the buttons, phone subscribed to everything:
 * contiguous blocks at 50 / s each reach the radio; the RTT stream is the
 * same as without a phone. */
/* The same BOTH session on a fixed timeline; returns a copy of the RTT
 * stream (caller frees). subs 0: no phone. radio_line: the radio logs it
 * 12.3 s after the start press (NULL: nothing). */
static void radio_line_cb(void *ctx) { sim_radio_log((const char *)ctx); }

static uint8_t *both_session_subs(uint8_t subs, const char *radio_line, size_t *n)
{
    scen_boot(false);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    scen_run_until(1000);
    CHECK(advertising());
    if (subs)
        CHECK(sim_phone_connect(247, 16, subs));
    scen_run_until(1100);
    if (radio_line)
        sim_at(sim_now_ms() + 12300, radio_line_cb, (void *)(uintptr_t)radio_line);
    for (int i = 0; i < 3; i++)
        scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_BOTH_CARD);
    sim_stream_clear();
    scen_tap(1);
    scen_run_ms(600);
    sim_set_thumbs(true);
    sim_set_finger(true);
    scen_run_ms(26000);
    CHECK(is_running());
    sim_set_thumbs(false);
    sim_set_finger(false);
    scen_run_ms(5000);
    CHECK(is_summary());
    size_t len;
    const uint8_t *d = sim_stream_data(&len);
    uint8_t *copy = malloc(len ? len : 1);
    if (copy && len)
        memcpy(copy, d, len);
    *n = len;
    return copy;
}

static uint8_t *both_session_fixed(bool phone_on, size_t *n)
{
    return both_session_subs(phone_on ? SUB_ALL : 0u, NULL, n);
}

TEST(streaming_both)
{
    size_t n0, n1;
    uint8_t *rtt0 = both_session_fixed(false, &n0);
    CHECK_EQ(sim_radio_info()->ecg_blocks, 0);
    uint8_t *rtt1 = both_session_fixed(true, &n1);
    /* the phone changes nothing on RTT, byte for byte */
    CHECK(n0 > 50000);
    CHECK_EQ(n1, n0);
    CHECK(rtt0 && rtt1 && memcmp(rtt0, rtt1, n0 < n1 ? n0 : n1) == 0);
    free(rtt0);
    free(rtt1);
    const sim_radio_info_t *ri = sim_radio_info();
    scen_decoded_t d = scen_decode_stream();
    printf("  radio: %u ECG %u PPG blocks, gaps %u/%u, runs %u/%u, %u events; RTT %u/%u\n",
           ri->ecg_blocks, ri->ppg_blocks, ri->ecg_gaps, ri->ppg_gaps, ri->ecg_runs, ri->ppg_runs,
           ri->events, d.ecg_blocks, d.ppg_blocks);
    CHECK_EQ(ri->ecg_gaps, 0);
    CHECK_EQ(ri->ppg_gaps, 0);
    CHECK_EQ(ri->ecg_runs, 1);
    CHECK_EQ(ri->ppg_runs, 1);
    CHECK_EQ(ri->seq_gaps, 0);
    CHECK_EQ(ri->crc_errors, 0);
    CHECK_EQ(ri->unsolicited, 0);
    CHECK_EQ(ri->ecg_blocks, d.ecg_blocks);
    CHECK_EQ(ri->ppg_blocks, d.ppg_blocks);
    double secs = (double)(ri->ecg_last_us - ri->ecg_first_us) / 1e6;
    CHECK_NEAR(ri->ecg_blocks / secs, 50.0, 1.0);
    CHECK_NEAR(ri->ppg_blocks / secs, 50.0, 1.0);
    CHECK(ri->events_by_kind[STREAM_EV_R_PEAK] > 15 && ri->events_by_kind[STREAM_EV_PULSE] > 10);
    /* the RTT stream has no trace of the radio */
    CHECK_EQ(d.bad_cobs + d.bad_crc + d.bad_len + d.seq_gaps, 0);
    CHECK_EQ(d.ecg_index_gaps + d.ppg_index_gaps, 0);
    CHECK_EQ(stream_drops(), 0);
    CHECK_EQ(radio_info()->data_drops, 0);
    CHECK(scen.max_step_acq_us <= 20000);
    CHECK_EQ(hal_ecg_overruns(), 0);
    CHECK_EQ(hal_ppg_overruns(), 0);
    /* only what is subscribed: ECG alone */
    sim_phone_subscribe(LINK_SUB_ECG);
    scen_run_ms(50);
    uint32_t ppg0 = sim_radio_info()->ppg_blocks, ecg0 = sim_radio_info()->ecg_blocks;
    scen_tap(1);
    CHECK_EQ(scen_screen(), UI_SCREEN_BOTH_CARD);
    scen_tap(1);
    scen_run_ms(600);
    sim_set_thumbs(true);
    sim_set_finger(true);
    scen_run_ms(5000);
    CHECK_EQ(sim_radio_info()->ppg_blocks, ppg0);
    CHECK(sim_radio_info()->ecg_blocks > ecg0 + 100);
    CHECK_EQ(sim_radio_info()->unsolicited, 0);
    /* a client subscribed to Heart Rate only still gets the beat events */
    uint32_t r0 = sim_radio_info()->events_by_kind[STREAM_EV_R_PEAK];
    sim_phone_subscribe(LINK_SUB_HR);
    scen_run_ms(5000);
    CHECK(sim_radio_info()->events_by_kind[STREAM_EV_R_PEAK] > r0 + 3);
    /* at most the frames already queued before the MCU saw the change */
    CHECK(sim_radio_info()->unsolicited <= 2);
    sim_set_thumbs(false);
    sim_set_finger(false);
    scen_run_ms(4000);
    end_checks();
}

TEST(phone_commands)
{
    boot_connected(SUB_ALL);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    /* the display goes off; a phone START wakes it on the ECG screen */
    CHECK(scen_run_cond(display_off, UI_DISPLAY_TIMEOUT_S * 1000 + 2000));
    CHECK(connected()); /* the display timeout keeps the connection */
    CHECK_EQ(cmd2(LINK_CMD_START, 1), LINK_RES_OK);
    scen_run_ms(200);
    CHECK_EQ(app_debug()->pwr, APP_PWR_AWAKE);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG);
    CHECK(scen_lit_pixels() > 100);
    CHECK_EQ(cmd2(LINK_CMD_START, 2), LINK_RES_BUSY);          /* already running */
    CHECK_EQ(cmd_set(SET_ITEM_GAIN, GAIN_HI), LINK_RES_BUSY);   /* applied at start */
    CHECK_EQ(cmd_set(SET_ITEM_SWEEP, SWEEP_6S), LINK_RES_OK);   /* display only */
    sim_set_thumbs(true);
    CHECK(scen_run_cond(is_running, 6000));
    CHECK_EQ(cmd1(LINK_CMD_GET_STATUS), LINK_RES_OK);
    const sim_link_frame_t *last = sim_radio_log_get(sim_radio_log_count() - 1);
    CHECK_EQ(last->type, LINK_STATUS); /* after the ACK and the event */
    CHECK_EQ(last->p[1], 1);
    CHECK_EQ(last->p[2], UI_MEAS_RUNNING);
    scen_run_ms(12000);
    CHECK(sim_radio_info()->last_status[11] > 50); /* hr */
    CHECK_EQ(cmd1(LINK_CMD_STOP), LINK_RES_OK);
    scen_run_ms(100);
    CHECK_EQ(scen_screen(), UI_SCREEN_SUMMARY);
    CHECK(!acq_active());
    sim_set_thumbs(false);
    CHECK_EQ(cmd1(LINK_CMD_STOP), LINK_RES_OK); /* idempotent */
    /* SET: Settings rules, deferred save */
    uint32_t w0 = sim_facts()->nv_writes;
    CHECK_EQ(cmd_set(SET_ITEM_GAIN, GAIN_HI), LINK_RES_OK);
    CHECK_EQ(cmd_set(SET_ITEM_RLD, RLD_LEFT), LINK_RES_OK);
    CHECK_EQ(cmd_set(SET_ITEM_RLD, 9), LINK_RES_BAD_ARG);
    CHECK_EQ(cmd_set(SET_ITEM_COUNT, 0), LINK_RES_BAD_ARG);
    CHECK_EQ(app_settings()->gain, GAIN_HI);
    CHECK_EQ(app_settings()->rld, RLD_LEFT);
    CHECK_EQ(app_settings()->sweep, SWEEP_6S);
    scen_run_ms(1000);
    CHECK_EQ(sim_facts()->nv_writes, w0); /* not yet: waits for the changes to stop */
    scen_run_ms(2500);
    CHECK_EQ(sim_facts()->nv_writes, w0 + 1);
    settings_t s;
    CHECK(settings_load(&s) == SETTINGS_LOADED);
    CHECK(s.gain == GAIN_HI && s.rld == RLD_LEFT && s.sweep == SWEEP_6S);
    /* malformed and unknown */
    CHECK_EQ(cmd2(LINK_CMD_START, 7), LINK_RES_BAD_ARG);
    CHECK_EQ(cmd1(0x42), LINK_RES_UNKNOWN);
    uint8_t too_long[3] = {LINK_CMD_STOP, 1, 2};
    CHECK_EQ(phone(too_long, 3), LINK_RES_BAD_ARG);
    /* SET_TIME: 2026-11-12 09:41:00 UTC, +60 min -> 10:41 */
    uint32_t t = 1794476460u;
    uint8_t st[7] = {LINK_CMD_SET_TIME, (uint8_t)t, (uint8_t)(t >> 8), (uint8_t)(t >> 16),
                     (uint8_t)(t >> 24), 60, 0};
    CHECK_EQ(phone(st, 7), LINK_RES_OK);
    scen_tap(2); /* any screen; Home has the clock */
    scen_press(2, 2100);
    scen_run_ms(200);
    CHECK(scen_m()->time_valid);
    CHECK_EQ(scen_m()->hour, 10);
    CHECK_EQ(scen_m()->minute, 41);
    CHECK(sim_radio_info()->last_status[4] & LINK_ST_TIME_VALID);
    scen_shot("radio_home_time_connected");
    /* the clock survives STANDBY (RTC) */
    scen_power_off();
    scen_run_ms(300);
    CHECK(in_standby());
    CHECK(scen_radio_off());
    scen_run_ms(10ull * 60 * 1000);
    scen_tap(2);
    scen_run_ms(300);
    CHECK(scen_m()->time_valid);
    CHECK_EQ(scen_m()->hour, 10);
    CHECK_NEAR(scen_m()->minute, 51, 1);
    end_checks();
}

static int g_cmd_seq;
static void start_ecg_cb(void *ctx)
{
    (void)ctx;
    uint8_t c[2] = {LINK_CMD_START, 1};
    sim_set_battery_mv(3480); /* the gauge alert: read in the pass this callback starts */
    g_cmd_seq = sim_phone_command(c, 2); /* on the wire for 0.15 ms: the pass after */
}

TEST(refusals)
{
    boot_connected(SUB_ALL);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    /* USB: ECG and BOTH refused, the USB screen shows as for the button */
    sim_set_usb(true);
    scen_run_ms(300);
    CHECK_EQ(cmd2(LINK_CMD_START, 1), LINK_RES_REFUSED_USB);
    scen_run_ms(100);
    CHECK_EQ(scen_screen(), UI_SCREEN_ECG);
    CHECK_EQ(scen_live_state(), UI_MEAS_USB_BLOCKED);
    CHECK(!sim_pin(PIN_MEASURE_REQ) && !sim_pin(PIN_ANA_EN));
    CHECK_EQ(cmd2(LINK_CMD_START, 3), LINK_RES_REFUSED_USB);
    CHECK_EQ(cmd2(LINK_CMD_START, 2), LINK_RES_OK); /* PPG may run on USB */
    scen_run_ms(500);
    CHECK(acq_active());
    CHECK_EQ(cmd1(LINK_CMD_STOP), LINK_RES_OK);
    scen_run_ms(100);
    sim_set_usb(false);
    scen_run_ms(300);
    /* low battery */
    sim_set_battery_mv(3480);
    scen_run_ms(11000);
    CHECK(scen_m()->batt_valid && scen_m()->batt_pct < BATT_WARN_PCT);
    CHECK(scen_radio_off()); /* low battery: radio off as well */
    sim_set_battery_mv(3900);
    scen_run_ms(11000);
    CHECK(scen_run_cond(advertising, 2000));
    CHECK(sim_phone_connect(247, 16, SUB_ALL));
    scen_run_ms(20);
    /* The radio goes off with a low battery, so REFUSED_BATT is only seen by
     * a START that reaches the MCU in the pass right after the low reading
     * (radio.c answers what it has received before it shuts down). The
     * callback drops the battery (gauge alert: the app reads it in the pass
     * the callback wakes) and sends the command, which lands after that
     * pass's radio poll. */
    g_cmd_seq = -1;
    sim_at_us(sim_now_us() + 400u, start_ecg_cb, NULL);
    scen_run_ms(20);
    CHECK(app_debug()->batt_pct < BATT_WARN_PCT);
    uint8_t r = 0xFF;
    CHECK(g_cmd_seq >= 0 && sim_radio_ack_of((uint8_t)g_cmd_seq, &r));
    CHECK_EQ(r, LINK_RES_REFUSED_BATT);
    CHECK(scen_run_cond(radio_off, 200));
    CHECK(!acq_active());
    sim_set_battery_mv(3900);
    scen_run_ms(11000);
    CHECK(scen_run_cond(advertising, 2000));
    CHECK(sim_phone_connect(247, 16, SUB_ALL));
    scen_run_ms(20);
    /* LED DAC missing (since boot, as in scen_faults.c): PPG start is a FAULT */
    sim_i2c_set_present(I2C_ADDR_MCP4728, false);
    scen_reboot();
    CHECK(scen_run_cond(advertising, 1000));
    CHECK(sim_phone_connect(247, 16, SUB_ALL));
    scen_run_ms(20);
    CHECK_EQ(cmd2(LINK_CMD_START, 2), LINK_RES_FAULT);
    scen_run_ms(100);
    CHECK_EQ(scen_live_state(), UI_MEAS_FAULT);
    sim_i2c_set_present(I2C_ADDR_MCP4728, true);
    CHECK_EQ(cmd1(LINK_CMD_STOP), LINK_RES_OK);
    scen_run_ms(100);
    CHECK_EQ(scen_screen(), UI_SCREEN_PPG_CARD);
    /* Diagnostics shown: BUSY */
    scen_press(2, 2100);
    for (int i = 0; i < 4 + SET_ITEM_COUNT; i++)
        scen_tap(2);
    scen_press_both(2300);
    scen_run_ms(300);
    CHECK_EQ(scen_screen(), UI_SCREEN_DIAG);
    CHECK_EQ(cmd2(LINK_CMD_START, 1), LINK_RES_BUSY);
    scen_tap(1);
    CHECK_EQ(scen_screen(), UI_SCREEN_ABOUT);
    end_checks();
}

/* USB plugged during a phone-started ECG record: the same as with the
 * button (scen_usb.c usb_mid_record). */
TEST(usb_during_phone_session)
{
    boot_connected(SUB_ALL);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    CHECK_EQ(cmd2(LINK_CMD_START, 1), LINK_RES_OK);
    scen_run_ms(500);
    sim_set_thumbs(true);
    CHECK(scen_run_cond(is_running, 6000));
    scen_run_ms(15000);
    CHECK(sim_rail(SIM_RAIL_BODY_EN));
    sim_set_usb(true);
    CHECK(!sim_pin(PIN_MEASURE_REQ)); /* the ISR */
    scen_run_ms(2);
    CHECK(scen_analog_all_off());
    scen_run_ms(200);
    CHECK_EQ(scen_m()->overlay, UI_OVERLAY_USB_STOPPED);
    CHECK(sim_radio_info()->last_status[4] & LINK_ST_USB);
    CHECK_EQ(cmd2(LINK_CMD_START, 2), LINK_RES_BUSY); /* an overlay is shown */
    CHECK(scen_run_cond(is_summary, 3000));
    scen_run_ms(100);
    CHECK(scen_m()->summary.stopped_by_usb);
    CHECK(scen_m()->summary.valid);
    CHECK_NEAR(scen_m()->summary.hr_avg, tc.hr_bpm, 2);
    CHECK_EQ(cmd2(LINK_CMD_START, 1), LINK_RES_REFUSED_USB);
    CHECK(connected());
    sim_set_thumbs(false);
    sim_set_usb(false);
    scen_run_ms(300);
    end_checks();
}

TEST(advertising_timeout_and_button)
{
    scen_boot_splash(false);
    CHECK(scen_run_cond(advertising, 1000));
    uint64_t t0 = sim_now_ms();
    scen_skip_boot_screen(); /* the skipping press leaves the radio alone */
    CHECK(scen_run_cond(radio_off, LINK_ADV_TOTAL_SECONDS * 1000 + 2000));
    uint64_t dt = sim_now_ms() - t0;
    printf("  advertising stopped after %llu ms\n", (unsigned long long)dt);
    CHECK(dt >= LINK_ADV_TOTAL_SECONDS * 1000 - 100 && dt <= LINK_ADV_TOTAL_SECONDS * 1000 + 200);
    CHECK_EQ(sim_radio_info()->conn_state, LINK_CS_IDLE);
    CHECK_EQ(sim_radio_info()->shutdowns, 1);
    CHECK_EQ(sim_radio_info()->clean_offs, 1);
    CHECK(scen_radio_off());
    CHECK(sim_radio_info()->ctrl_ops[LINK_RC_ADV_START] == 1);
    CHECK_EQ(sim_radio_info()->conn_state, LINK_CS_IDLE);
    scen_run_ms(30000);
    CHECK(radio_off()); /* stays off */
    CHECK(display_off());
    scen_tap(2); /* wakes the display and the radio */
    CHECK(scen_run_cond(advertising, 1000));
    CHECK_EQ(sim_radio_info()->power_ups, 2);
    /* a connection never times out; a disconnect restarts the window */
    CHECK(sim_phone_connect(185, 24, LINK_SUB_STATUS));
    scen_run_ms(200ull * 1000);
    CHECK(connected());
    sim_phone_disconnect();
    scen_run_ms(100);
    CHECK(advertising());
    /* boot, button; after the disconnect the radio advertises by itself */
    CHECK_EQ(sim_radio_info()->ctrl_ops[LINK_RC_ADV_START], 2);
    CHECK_EQ(sim_radio_info()->conn_state, LINK_CS_ADV_FAST);
    end_checks();
}

TEST(ble_off_setting)
{
    scen_boot(false);
    CHECK(scen_run_cond(advertising, 1000));
    for (int i = 0; i < 4; i++)
        scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_SETTINGS);
    for (int i = 0; i < SET_ITEM_BLE; i++)
        scen_tap(2);
    CHECK_EQ(scen_m()->settings_cursor, SET_ITEM_BLE);
    scen_shot("settings_ble_on");
    scen_tap(1);
    CHECK_EQ(app_settings()->ble, BLE_OFF);
    scen_shot("settings_ble_off");
    CHECK(scen_run_cond(radio_off, 200));
    CHECK_EQ(sim_radio_info()->clean_offs, 1);
    CHECK(scen_radio_off());
    scen_tap(2); /* leave Settings: saved */
    scen_run_ms(500);
    scen_reboot();
    scen_run_ms(3000);
    CHECK_EQ(app_settings()->ble, BLE_OFF);
    CHECK(scen_radio_off());
    CHECK_EQ(sim_radio_info()->power_ups, 1);
    CHECK_EQ(scen_m()->ble_state, UI_BLE_OFF);
    /* the phone can turn it off as well (and the ACK goes out first) */
    for (int i = 0; i < 4; i++)
        scen_tap(2);
    for (int i = 0; i < SET_ITEM_BLE; i++)
        scen_tap(2);
    scen_tap(1);
    CHECK(scen_run_cond(advertising, 1000));
    CHECK(sim_phone_connect(247, 16, SUB_ALL));
    scen_run_ms(20);
    CHECK_EQ(cmd_set(SET_ITEM_BLE, BLE_OFF), LINK_RES_OK);
    CHECK(scen_run_cond(radio_off, 200));
    CHECK_EQ(sim_radio_info()->clean_offs, 2);
    end_checks();
}

TEST(standby_handshake)
{
    for (int no_ack = 0; no_ack < 2; no_ack++) {
        scen_boot(false);
        if (no_ack) {
            sim_radio_cfg_t c = sim_radio_defaults();
            c.ack_shutdown = false;
            sim_radio_config(&c);
            scen_reboot();
        }
        CHECK(scen_run_cond(advertising, 1000));
        CHECK(sim_phone_connect(247, 16, SUB_ALL));
        scen_run_ms(100);
        scen_power_off();
        scen_run_ms(200);
        CHECK(in_standby());
        CHECK(scen_radio_off());
        const sim_radio_info_t *ri = sim_radio_info();
        printf("  %s: shutdown -> power cut %llu us\n", no_ack ? "no ACK" : "ACK",
               (unsigned long long)ri->last_shutdown_wait_us);
        CHECK_EQ(ri->shutdowns, 1);
        CHECK_EQ(ri->clean_offs, 1);
        if (no_ack)
            CHECK(ri->last_shutdown_wait_us >= LINK_SHUTDOWN_WAIT_MS * 1000u - 1000u);
        else
            CHECK(ri->last_shutdown_wait_us < 5000u);
        size_t n = sim_radio_trace_count();
        CHECK_EQ(sim_radio_trace_get(n - 2, NULL), SIM_RT_UART_OFF);
        CHECK_EQ(sim_radio_trace_get(n - 1, NULL), SIM_RT_RADIO_EN_LOW);
        end_checks();
    }
}

/* Garbage, corrupted and dropped bytes from the radio during a connected
 * BOTH session: no crash (ASan / UBSan), no stall, the session and the link
 * carry on, and commands work afterwards. */
static void noise(void *ctx)
{
    (void)ctx;
    uint8_t b[300];
    int n = 1 + rand() % (int)sizeof b;
    for (int i = 0; i < n; i++)
        b[i] = (uint8_t)(rand() % 5 == 0 ? 0 : rand());
    sim_radio_send_raw(b, (size_t)n);
    if (rand() % 3 == 0)
        sim_radio_corrupt_to_mcu((uint32_t)(1 + rand() % 4), (uint8_t)(1u << (rand() % 8)));
    if (rand() % 4 == 0)
        sim_radio_drop_to_mcu((uint32_t)(1 + rand() % 3));
    if (rand() % 4 == 0)
        sim_radio_log("noise log");
    if (rand() % 6 == 0) {
        uint8_t c[1 + 15];
        size_t m = (size_t)(rand() % 16);
        for (size_t i = 0; i < m; i++)
            c[i] = (uint8_t)rand();
        if (m && rand() % 2)
            c[0] = LINK_CMD_GET_STATUS;
        sim_phone_command(c, m);
    }
    if (rand() % 5 == 0)
        sim_radio_corrupt_to_radio(1, 0x10);
}

TEST(corrupted_input)
{
    srand(777);
    boot_connected(SUB_ALL);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    for (int i = 0; i < 3; i++)
        scen_tap(2);
    scen_tap(1);
    scen_run_ms(600);
    sim_set_thumbs(true);
    sim_set_finger(true);
    CHECK(scen_run_cond(is_running, 6000));
    uint64_t t = sim_now_ms();
    for (int k = 0; k < 400; k++)
        sim_at(t + 10 + (uint64_t)k * 37, noise, NULL);
    scen_run_ms(16000);
    CHECK(is_running());
    CHECK(connected());
    const radio_info_t *r = radio_info();
    hal_uart_stats_t us;
    hal_uart_stats(&us);
    printf("  rx: %u frames, crc %u, bad %u, gaps %u, unknown %u; cmds %u (%u not OK); uart "
           "%u bytes, overruns %u\n", r->rx.frames, r->rx.crc_errors, r->rx.bad_frames,
           r->rx.seq_gaps, r->rx.unknown_types, r->cmds, r->cmd_errors, us.rx_bytes, us.rx_overruns);
    CHECK(r->rx.crc_errors + r->rx.bad_frames > 50);
    CHECK(scen.max_step_acq_us <= 20000);
    CHECK_EQ(hal_ecg_overruns(), 0);
    CHECK_EQ(hal_ppg_overruns(), 0);
    CHECK(sim_radio_info()->ecg_blocks > 600);
    CHECK_EQ(cmd1(LINK_CMD_GET_STATUS), LINK_RES_OK);
    CHECK_EQ(cmd1(LINK_CMD_STOP), LINK_RES_OK);
    sim_set_thumbs(false);
    sim_set_finger(false);
    scen_run_ms(500);
    end_checks();
}

/* The UART cannot keep up (line frozen): frames for the phone are dropped
 * and counted, the main loop and the acquisition do not notice, RTT is
 * unaffected, and the link recovers. */
TEST(tx_backlog)
{
    boot_connected(SUB_ALL);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    for (int i = 0; i < 3; i++)
        scen_tap(2);
    sim_stream_clear();
    scen_tap(1);
    scen_run_ms(600);
    sim_set_thumbs(true);
    sim_set_finger(true);
    CHECK(scen_run_cond(is_running, 6000));
    scen_run_ms(2000);
    sim_uart_stall_tx(true);
    scen_run_ms(3000);
    uint32_t drops = radio_info()->data_drops;
    printf("  link frozen 3 s: %u frames dropped, TX ring %u bytes\n", drops, HAL_UART_TX_RING);
    CHECK(drops > 250); /* ~150 frames / s, the ring holds ~0.15 s */
    sim_uart_stall_tx(false);
    scen_run_ms(3000);
    CHECK_EQ(radio_info()->data_drops, drops); /* recovered */
    CHECK(sim_radio_info()->ecg_gaps >= 1);       /* the phone sees the hole by index */
    CHECK(sim_radio_info()->seq_gaps >= 1);
    CHECK(is_running());
    CHECK(scen.max_step_acq_us <= 20000);
    CHECK_EQ(hal_ecg_overruns(), 0);
    CHECK_EQ(hal_ppg_overruns(), 0);
    scen_decoded_t d = scen_decode_stream();
    CHECK_EQ(d.seq_gaps + d.ecg_index_gaps + d.ppg_index_gaps, 0);
    CHECK_EQ(stream_drops(), 0);
    sim_set_thumbs(false);
    sim_set_finger(false);
    scen_run_ms(4000);
    end_checks();
}

static bool on_radio_page(void)
{
    return scen_screen() == UI_SCREEN_DIAG && strstr(scen_m()->diag[0], "RADIO") != NULL;
}

static void to_radio_page(void)
{
    for (int i = 0; i < 4 + SET_ITEM_COUNT; i++)
        scen_tap(2);
    CHECK_EQ(scen_screen(), UI_SCREEN_ABOUT);
    scen_press_both(2300);
    scen_run_ms(300);
    for (int i = 0; i < 5; i++)
        scen_tap(2);
    scen_run_ms(300);
    CHECK(on_radio_page());
}

TEST(program_mode)
{
    scen_boot(false);
    CHECK(scen_run_cond(advertising, 1000));
    to_radio_page();
    scen_shot("radio_diag");
    /* the radio is flashed: it runs no firmware (no HELLO ever) */
    sim_radio_cfg_t c = sim_radio_defaults();
    c.present = false;
    sim_radio_config(&c);
    scen_tap(1);
    scen_run_ms(300);
    CHECK(radio_program_mode());
    CHECK_EQ(radio_state(), RADIO_PROGRAM);
    CHECK(strstr(scen_m()->diag[0], "PROGRAM MODE ON") != NULL);
    scen_shot("radio_diag_program");
    size_t frames = sim_radio_info()->frames;
    uint32_t ups = sim_radio_info()->power_ups;
    /* 15 minutes: no timeout, no display-off, no standby, power held */
    for (int i = 0; i < 90; i++) {
        scen_run_ms(10000);
        CHECK(sim_pin(PIN_RADIO_EN));
        CHECK_EQ(app_debug()->pwr, APP_PWR_AWAKE);
    }
    CHECK_EQ(sim_radio_info()->frames, frames); /* the MCU sent nothing */
    CHECK_EQ(sim_radio_info()->power_ups, ups); /* never cycled */
    CHECK_EQ(radio_state(), RADIO_PROGRAM);
    CHECK(on_radio_page());
    /* new firmware starts: its HELLO and log lines show, still held */
    sim_radio_config(&c);
    c.present = true;
    c.fw[0] = 2;
    sim_radio_config(&c);
    sim_radio_self_reset();
    scen_run_ms(300);
    CHECK(log_has("ble: HELLO fw 2.2.3"));
    CHECK(log_has("radio: CH585 link up"));
    CHECK_EQ(radio_state(), RADIO_PROGRAM);
    CHECK_EQ(sim_radio_info()->frames, frames);
    /* toggle off: normal power-down, then the normal policy brings it back */
    scen_tap(1);
    scen_run_ms(20);
    CHECK(!radio_program_mode());
    CHECK(scen_run_cond(advertising, 1000));
    CHECK_EQ(sim_radio_info()->shutdowns, 1);
    CHECK(on_radio_page());
    /* leaving the page ends program mode too */
    scen_tap(1);
    scen_run_ms(100);
    CHECK(radio_program_mode());
    scen_tap(2);
    scen_run_ms(20);
    CHECK(!radio_program_mode());
    CHECK(scen_run_cond(advertising, 1000));
    /* and so does a power-off */
    for (int i = 0; i < 5; i++)
        scen_tap(2);
    CHECK(on_radio_page());
    scen_tap(1);
    scen_run_ms(100);
    CHECK(radio_program_mode());
    scen_power_off();
    scen_run_ms(300);
    CHECK(in_standby());
    CHECK(scen_radio_off());
    CHECK(!radio_program_mode());
    end_checks();
}

/* A phone polling GET_STATUS keeps the card out of STANDBY; a silent one
 * does not. The radio rebooting by itself is set up again. */
TEST(phone_keeps_awake_and_radio_reset)
{
    boot_connected(LINK_SUB_STATUS);
    for (int i = 0; i < 12; i++) { /* 12 minutes */
        scen_run_ms(60000);
        CHECK_EQ(cmd1(LINK_CMD_GET_STATUS), LINK_RES_OK);
    }
    CHECK(display_off());
    CHECK(connected());
    uint64_t t0 = sim_now_ms();
    CHECK(scen_run_cond(in_standby, UI_STANDBY_TIMEOUT_S * 1000 + 2000));
    CHECK_NEAR((double)(sim_now_ms() - t0), UI_STANDBY_TIMEOUT_S * 1000.0, 1500.0);
    CHECK(scen_radio_off());
    scen_tap(2);
    CHECK(scen_run_cond(advertising, 1000));
    CHECK(sim_phone_connect(247, 16, LINK_SUB_STATUS));
    CHECK(scen_run_cond(connected, 100));
    uint32_t info0 = sim_radio_info()->ctrl_ops[LINK_RC_SET_INFO];
    sim_radio_self_reset();
    scen_run_ms(200);
    CHECK(advertising());
    CHECK_EQ(sim_radio_info()->ctrl_ops[LINK_RC_SET_INFO], info0 + 1);
    CHECK(log_has("radio restarted by itself"));
    CHECK_EQ(radio_info()->restarts, 1);
    CHECK_EQ(radio_state(), RADIO_ADVERTISING); /* not a fault */
    /* a crashing radio: watchdog resets (reason 2) every ~0.6 s */
    sim_radio_cfg_t c = sim_radio_defaults();
    c.reset_reason = 2;
    sim_radio_config(&c);
    for (int k = 0; k < 5; k++) {
        sim_radio_self_reset();
        scen_run_ms(600);
    }
    CHECK_EQ(radio_info()->restarts, 6);
    CHECK_EQ(radio_state(), RADIO_ADVERTISING);
    CHECK(log_has("reset 2 (watchdog)"));
    CHECK_EQ(sim_radio_info()->power_ups, 2); /* never power-cycled by the MCU */
    end_checks();
}

/* ---- device log over BLE (Log characteristic) ------------------------------ */

typedef struct {
    const char *p;
    size_t n;
} span_t;

/* Lines of t (without '\n'), empty ones skipped; with no_radio also those
 * the MCU printed for the radio ("[t] radio: ..."). Caller frees. */
static span_t *split_lines(const char *t, bool no_radio, size_t *count)
{
    size_t cap = 16, n = 0;
    span_t *v = malloc(cap * sizeof *v);
    while (v && *t) {
        const char *e = strchr(t, '\n');
        size_t len = e ? (size_t)(e - t) : strlen(t);
        bool radio = no_radio && memmem(t, len, "] radio: ", 9) != NULL;
        if (len && !radio) {
            if (n == cap) {
                cap *= 2;
                span_t *nv = realloc(v, cap * sizeof *v);
                if (!nv)
                    break;
                v = nv;
            }
            v[n++] = (span_t){t, len};
        }
        t += len + (e ? 1u : 0u);
    }
    *count = n;
    return v;
}

typedef struct {
    bool ok;            /* every phone line is an RTT line (or a head of one), in order */
    size_t matched;     /* phone lines equal to an RTT line */
    size_t heads;       /* phone lines that are the start of an RTT line (cut short) */
    size_t radio;       /* "radio: " lines (the radio's own) */
    size_t skipped;     /* RTT lines missing on the phone, between its first and last */
    size_t after;       /* RTT lines after the phone's last line */
} mirror_t;

/* Compare what the phone got on Log with RTT channel 0. */
static mirror_t log_mirror(const char *rtt, const char *phone, bool heads_ok)
{
    mirror_t m = {true, 0, 0, 0, 0, 0};
    size_t nr, np;
    span_t *r = split_lines(rtt, true, &nr), *ph = split_lines(phone, false, &np);
    size_t j = 0;
    bool started = false;
    for (size_t i = 0; i < np && r && ph; i++) {
        span_t q = ph[i];
        if (q.n >= 7 && memcmp(q.p, "radio: ", 7) == 0) {
            m.radio++;
            continue;
        }
        size_t k = j;
        bool head = false;
        for (; k < nr; k++) {
            if (r[k].n == q.n && memcmp(r[k].p, q.p, q.n) == 0)
                break;
            if (heads_ok && q.n < r[k].n && memcmp(r[k].p, q.p, q.n) == 0) {
                head = true;
                break;
            }
        }
        if (k == nr) {
            printf("  phone line not on RTT (or out of order): \"%.*s\"\n", (int)q.n, q.p);
            m.ok = false;
            break;
        }
        if (started)
            m.skipped += k - j;
        started = true;
        if (head)
            m.heads++;
        else
            m.matched++;
        j = k + 1;
    }
    m.after = nr - j;
    free(r);
    free(ph);
    return m;
}

static size_t count_str(const char *hay, const char *needle)
{
    size_t n = 0;
    for (const char *q = hay; (q = strstr(q, needle)) != NULL; q += strlen(needle))
        n++;
    return n;
}

static void replace_all(char *t, const char *from, const char *to)
{
    size_t n = strlen(from);
    for (char *q = t; (q = strstr(q, from)) != NULL; q += n)
        memcpy(q, to, n); /* same length */
}

/* The fixed BOTH session with a phone subscribed to everything, without and
 * with Log: RTT (text and stream) is identical apart from the subscription
 * value in the "connected" line, the phone gets the same blocks, and the Log
 * text equals the RTT text from the connection on (minus the radio's lines,
 * which the radio tees itself). Unsubscribed: no STREAM_LOG at all. */
TEST(device_log_mirror)
{
    static const char radio_line[] = "ble: sent 1234 drop 0 q 3 busy 0 (test)";
    const uint8_t sub_log = SUB_ALL | LINK_SUB_LOG;
    size_t n0, n1;
    uint8_t *bin0 = both_session_subs(SUB_ALL, radio_line, &n0);
    char *txt0 = strdup(sim_log_text());
    sim_radio_info_t ri0 = *sim_radio_info();
    CHECK_EQ(ri0.log_frames, 0); /* no subscription, no STREAM_LOG frames */
    CHECK_EQ(ri0.by_type[STREAM_LOG], 0);
    CHECK_EQ(sim_phone_log_text(NULL)[0], 0);
    CHECK_EQ(radio_info()->log_lines + radio_info()->log_drops, 0);

    uint8_t *bin1 = both_session_subs(sub_log, radio_line, &n1);
    char *txt1 = strdup(sim_log_text());
    const sim_radio_info_t *ri1 = sim_radio_info();
    const radio_info_t *mi = radio_info();
    /* RTT: the stream byte for byte, the text but for "sub 0x3F" */
    CHECK(bin0 && bin1 && n0 > 50000 && n1 == n0 && memcmp(bin0, bin1, n0) == 0);
    CHECK(txt0 && txt1);
    if (txt0 && txt1) {
        CHECK_EQ(count_str(txt1, "sub 0x3F"), 1);
        replace_all(txt1, "sub 0x3F", "sub 0x1F");
        CHECK(strcmp(txt0, txt1) == 0);
    }
    /* block delivery unchanged while the log flows */
    CHECK_EQ(ri1->ecg_blocks, ri0.ecg_blocks);
    CHECK_EQ(ri1->ppg_blocks, ri0.ppg_blocks);
    CHECK_EQ(ri1->events, ri0.events);
    CHECK_EQ(ri1->status_frames, ri0.status_frames);
    CHECK_EQ(ri1->ecg_gaps + ri1->ppg_gaps + ri1->seq_gaps + ri1->crc_errors, 0);
    CHECK_EQ(ri1->unsolicited, 0);
    CHECK_EQ(ri1->oversize_notify, 0); /* every chunk <= 32 bytes */
    CHECK_EQ(mi->data_drops, 0);
    CHECK_EQ(link_tx_drops(), 0);
    CHECK(scen.max_step_acq_us <= 20000);
    /* the Log text */
    size_t plen;
    const char *ph = sim_phone_log_text(&plen);
    mirror_t m = log_mirror(sim_log_text(), ph, false);
    double secs = (double)(ri1->ecg_last_us - ri1->ecg_first_us) / 1e6;
    printf("  Log: %u lines (%u dropped) in %u frames, %u bytes (%.0f B/s over the session)\n",
           mi->log_lines, mi->log_drops, ri1->log_frames, ri1->log_bytes, ri1->log_bytes / secs);
    CHECK(m.ok);
    CHECK_EQ(m.skipped, 0);
    CHECK_EQ(m.heads, 0);
    CHECK_EQ(m.after, 0);
    CHECK_EQ(m.matched, mi->log_lines);
    CHECK(m.matched > 50);
    CHECK_EQ(mi->log_drops, 0);
    CHECK(plen && ph[plen - 1] == '\n');
    CHECK(strstr(ph, "] ecg: ") && strstr(ph, " rail=") && strstr(ph, "] ppg: ") && strstr(ph, " led="));
    /* the radio's line: once, from the radio itself; the MCU did not echo it */
    CHECK_EQ(m.radio, 1);
    CHECK_EQ(count_str(ph, "radio: ble: sent 1234 drop 0 q 3 busy 0 (test)\n"), 1);
    CHECK_EQ(count_str(ph, "] radio: "), 0);
    CHECK_EQ(count_str(sim_log_text(), "] radio: ble: sent 1234"), 1);
    free(bin0);
    free(bin1);
    free(txt0);
    free(txt1);
    /* Log unsubscribed: nothing more, while lines keep coming on RTT */
    sim_phone_subscribe(SUB_ALL);
    scen_run_ms(50);
    uint32_t f0 = sim_radio_info()->log_frames;
    size_t rtt0 = sim_log_len();
    scen_tap(1);
    CHECK_EQ(scen_screen(), UI_SCREEN_BOTH_CARD);
    scen_tap(1);
    scen_run_ms(600);
    sim_set_thumbs(true);
    sim_set_finger(true);
    scen_run_ms(5000);
    CHECK(sim_log_len() > rtt0 + 500);
    CHECK_EQ(sim_radio_info()->log_frames, f0);
    CHECK_EQ(sim_radio_info()->unsolicited, 0);
    sim_set_thumbs(false);
    sim_set_finger(false);
    scen_run_ms(4000);
    end_checks();
}

/* Back-pressure: (a) the UART frozen for 3 s during a BOTH session: lines
 * are dropped whole, what arrives is in order and never a tail without its
 * start, streaming recovers as without the log; (b) a ring with room for one
 * chunk only: the line is cut short, and the next line starts on a line of
 * its own. */
TEST(device_log_tx_stall)
{
    boot_connected(SUB_ALL | LINK_SUB_LOG);
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    for (int i = 0; i < 3; i++)
        scen_tap(2);
    sim_stream_clear();
    scen_tap(1);
    scen_run_ms(600);
    sim_set_thumbs(true);
    sim_set_finger(true);
    CHECK(scen_run_cond(is_running, 6000));
    scen_run_ms(2000);
    uint32_t ld0 = radio_info()->log_drops, ll0 = radio_info()->log_lines;
    sim_uart_stall_tx(true);
    scen_run_ms(3000);
    uint32_t drops = radio_info()->data_drops, ldrops = radio_info()->log_drops - ld0;
    printf("  link frozen 3 s: %u data frames and %u log lines dropped\n", drops, ldrops);
    CHECK(drops > 250);
    CHECK(ldrops >= 6); /* ~2 evidence lines / s */
    sim_uart_stall_tx(false);
    scen_run_ms(3000);
    CHECK_EQ(radio_info()->data_drops, drops); /* recovered */
    CHECK(radio_info()->log_lines > ll0 + 4);  /* and the log flows again */
    CHECK(is_running());
    CHECK(scen.max_step_acq_us <= 20000);
    CHECK_EQ(hal_ecg_overruns(), 0);
    CHECK_EQ(hal_ppg_overruns(), 0);
    scen_decoded_t d = scen_decode_stream();
    CHECK_EQ(d.seq_gaps + d.ecg_index_gaps + d.ppg_index_gaps, 0);
    CHECK_EQ(stream_drops(), 0);
    sim_set_thumbs(false);
    sim_set_finger(false);
    scen_run_ms(4000);
    mirror_t m = log_mirror(sim_log_text(), sim_phone_log_text(NULL), true);
    printf("  Log: %zu lines, %zu cut short, %zu missing\n", m.matched, m.heads, m.skipped);
    CHECK(m.ok); /* in order, no tails, no merged lines */
    CHECK_EQ(m.after, 0);
    CHECK(m.skipped >= ldrops - m.heads);
    CHECK(m.heads <= radio_info()->log_drops);
    CHECK_EQ(sim_radio_info()->oversize_notify, 0);

    /* (b) idle, connected; frozen line, ring filled (0x00 is no frame) up to
     * 60 free bytes behind the estimate's back: the next line (two chunks,
     * 40 + 31 wire bytes) loses its second chunk */
    scen_run_ms(1000);
    sim_phone_log_clear();
    uint32_t cut0 = radio_info()->log_drops;
    sim_uart_stall_tx(true);
    static uint8_t zeros[HAL_UART_TX_RING - 60];
    memset(zeros, 0, sizeof zeros);
    CHECK_EQ(hal_uart_write(zeros, sizeof zeros), sizeof zeros);
    sim_phone_subscribe((SUB_ALL | LINK_SUB_LOG) & ~LINK_SUB_HR); /* "ble: subscriptions ..." */
    scen_run_ms(2);
    CHECK_EQ(radio_info()->log_drops, cut0 + 1);
    sim_uart_stall_tx(false);
    scen_run_ms(20);
    sim_phone_subscribe(SUB_ALL | LINK_SUB_LOG);
    scen_run_ms(20);
    const char *ph = sim_phone_log_text(NULL);
    printf("  cut line on the phone: \"%s\"\n", ph);
    m = log_mirror(sim_log_text(), ph, true);
    CHECK(m.ok);
    CHECK_EQ(m.heads, 1);
    CHECK_EQ(m.matched, 1);
    CHECK_EQ(strlen(ph) > 33 ? ph[32] : 0, '\n'); /* the head is chunk 1; the '\n' starts chunk 2 */
    CHECK(strstr(ph + 33, "] ble: subscriptions mtu 247") == strchr(ph + 33, ']'));
    end_checks();
}

int main(void)
{
    RUN(power_up_hello_advertise);
    RUN(hello_missing_retry_fault);
    RUN(streaming_both);
    RUN(phone_commands);
    RUN(refusals);
    RUN(usb_during_phone_session);
    RUN(advertising_timeout_and_button);
    RUN(ble_off_setting);
    RUN(standby_handshake);
    RUN(corrupted_input);
    RUN(tx_backlog);
    RUN(program_mode);
    RUN(phone_keeps_awake_and_radio_reset);
    RUN(device_log_mirror);
    RUN(device_log_tx_stall);
    return TEST_RESULT();
}
