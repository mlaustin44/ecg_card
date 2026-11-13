#include "scen.h"

#include <stdio.h>
#include <string.h>

#include "acq.h"
#include "rails.h"
#include "stream.h"
#include "stream_fmt.h"

scen_stats_t scen;

static void check_step(void)
{
    bool ecg_acq = acq_active() && acq_status()->phase == ACQ_ON;
    if (sim_rail(SIM_RAIL_BODY_EN) && !ecg_acq)
        scen.body_en_outside_session++;
    if (sim_pin(PIN_MEASURE_REQ) && sim_usb_present())
        scen.measure_req_on_usb++;
    bool analog_needed = acq_active() || rails_analog_is_on();
    if (!analog_needed && (sim_pin(PIN_ANA_EN) || sim_pin(PIN_VLED_EN) || sim_pin(PIN_MEASURE_REQ) ||
                           sim_pin(PIN_BUCK_PS_SYNC)))
        scen.analog_outside++;
    double ma = sim_led_now_ma();
    if (ma > scen.max_led_ma)
        scen.max_led_ma = ma;
}

void scen_boot(bool keep_nv)
{
    static uint8_t nv[HAL_NV_MAX];
    if (keep_nv)
        sim_nv_get(nv, sizeof nv);
    sim_reset();
    sim_set_violation_mode(SIM_VIOL_ABORT);
    if (keep_nv)
        sim_nv_set(nv, sizeof nv);
    memset(&scen, 0, sizeof scen);
    app_init();
}

void scen_reboot(void)
{
    sim_reboot();
    app_init();
}

static void step(void)
{
    bool awake = app_debug()->pwr != APP_PWR_STANDBY;
    bool acq = acq_active();
    uint64_t t0 = sim_now_us();
    app_step();
    uint64_t dt = sim_now_us() - t0;
    if (awake && app_debug()->pwr != APP_PWR_STANDBY) {
        if (dt > scen.max_step_us)
            scen.max_step_us = dt;
        if ((acq || acq_active()) && dt > scen.max_step_acq_us)
            scen.max_step_acq_us = dt;
    }
    scen.steps++;
    check_step();
}

void scen_run_until(uint64_t t_ms)
{
    if (sim_now_ms() >= t_ms)
        return;
    sim_clear_stop();
    sim_stop_at(t_ms);
    while (!sim_should_stop())
        step();
    sim_clear_stop();
}

void scen_run_ms(uint64_t ms) { scen_run_until(sim_now_ms() + ms); }

bool scen_run_cond(bool (*cond)(void), uint64_t timeout_ms)
{
    uint64_t end = sim_now_ms() + timeout_ms;
    sim_clear_stop();
    sim_stop_at(end);
    while (!sim_should_stop() && !cond())
        step();
    sim_clear_stop();
    sim_stop_at(UINT64_MAX / 1000);
    return cond();
}

void scen_press(int button, uint64_t hold_ms)
{
    uint64_t t = sim_now_ms() + 1;
    sim_at_button(t, button, true);
    sim_at_button(t + hold_ms, button, false);
    scen_run_until(t + hold_ms + 60);
}

void scen_tap(int button) { scen_press(button, 100); }

void scen_press_both(uint64_t hold_ms)
{
    uint64_t t = sim_now_ms() + 1;
    sim_at_button(t, 1, true);
    sim_at_button(t + 5, 2, true);
    sim_at_button(t + hold_ms, 1, false);
    sim_at_button(t + hold_ms + 5, 2, false);
    scen_run_until(t + hold_ms + 70);
}

const ui_model_t *scen_m(void) { return app_model(); }
ui_screen_t scen_screen(void) { return app_model()->screen; }
ui_meas_state_t scen_live_state(void) { return app_model()->live.state; }

int scen_lit_pixels(void)
{
    static uint8_t img[64][128];
    sim_oled_image(img);
    int n = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 128; x++)
            n += img[y][x];
    return n;
}

void scen_shot(const char *name)
{
    char path[256];
    snprintf(path, sizeof path, "build/app_shots/%s.png", name);
    if (!sim_oled_save_png(path, 4))
        printf("  (could not write %s)\n", path);
}

bool scen_analog_all_off(void)
{
    static const board_pin_t p[] = {PIN_ANA_EN, PIN_VLED_EN, PIN_BUCK_PS_SYNC, PIN_MEASURE_REQ,
                                    PIN_FAST_RESTORE, PIN_GAIN_HI, PIN_RLD_L_EN, PIN_RLD_R_EN,
                                    PIN_RADIO_EN};
    for (size_t i = 0; i < sizeof p / sizeof p[0]; i++)
        if (sim_pin(p[i]))
            return false;
    return sim_led_blank() && sim_led_now_ma() == 0.0 && !hal_acq_running() && !sim_clock_fast();
}

/* ---- stream decode ------------------------------------------------------- */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

scen_decoded_t scen_decode_stream(void)
{
    scen_decoded_t d;
    memset(&d, 0, sizeof d);
    size_t n;
    const uint8_t *s = sim_stream_data(&n);
    size_t i = 0;
    bool have_seq = false, have_ecg = false, have_ppg = false;
    uint8_t seq = 0;
    uint32_t next_ecg = 0, next_ppg = 0;
    static uint8_t f[1024];
    while (i < n) {
        size_t j = i;
        while (j < n && s[j] != 0)
            j++;
        if (j == n)
            break; /* incomplete tail */
        size_t len = cobs_decode(s + i, j - i, f, sizeof f);
        i = j + 1;
        if (len == (size_t)-1) { d.bad_cobs++; continue; }
        if (len < 6 || (size_t)rd16(f + 2) + 6u != len) { d.bad_len++; continue; }
        size_t pl = rd16(f + 2);
        if (crc16_ccitt_false(f, 4 + pl, 0xFFFFu) != rd16(f + 4 + pl)) { d.bad_crc++; continue; }
        d.frames++;
        if (have_seq && f[1] != (uint8_t)(seq + 1u))
            d.seq_gaps++;
        seq = f[1];
        have_seq = true;
        const uint8_t *p = f + 4;
        switch (f[0]) {
        case STREAM_ECG_BLOCK: {
            if (pl != 25) { d.bad_len++; break; }
            uint32_t st = rd32(p);
            if (st == 0) { d.runs++; have_ecg = true; }
            else if (!have_ecg || st != next_ecg) d.ecg_index_gaps++;
            next_ecg = st + 10;
            d.ecg_blocks++;
            if (p[4] & 2) d.ecg_settling_blocks++;
            if (p[4] & 4) d.ecg_leadoff_blocks++;
            break;
        }
        case STREAM_PPG_BLOCK: {
            if (pl != 15) { d.bad_len++; break; }
            uint32_t st = rd32(p);
            if (st == 0) { have_ppg = true; if (!have_ecg || next_ecg == 0) d.runs += 0; }
            else if (!have_ppg || st != next_ppg) d.ppg_index_gaps++;
            next_ppg = st + 2;
            d.ppg_blocks++;
            if (p[6] & 4) d.ppg_nofinger_blocks++;
            break;
        }
        case STREAM_EVENT:
            switch (p[0]) {
            case STREAM_EV_R_PEAK:
                d.r_peaks++;
                if (rd16(p + 5)) { d.hr_n++; d.hr_sum += rd16(p + 5); }
                break;
            case STREAM_EV_PULSE:
                d.pulses++;
                if (rd16(p + 7)) { d.pat_n++; d.pat_sum_ms += rd16(p + 7); }
                break;
            case STREAM_EV_LEAD: d.leads++; break;
            case STREAM_EV_STATE: d.states++; break;
            case STREAM_EV_BUTTON: d.buttons++; break;
            default: break;
            }
            break;
        default: break;
        }
    }
    return d;
}
