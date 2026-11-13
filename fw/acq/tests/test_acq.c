/* acq.c against the simulator (ABORT mode) with synthetic ECG / PPG. */
#include <stdlib.h>
#include <string.h>

#include "acq.h"
#include "mcp4728.h"
#include "rails.h"
#include "settings.h"
#include "sim.h"
#include "test.h"
#include "testsrc.h"

static struct {
    uint32_t ecg_blocks, ppg_blocks, beats, pulses, contacts, settles;
    uint32_t next_ecg, next_ppg;
    bool ecg_gap, ppg_gap;
    uint32_t pat_sum, pat_n;
    uint64_t fr_rise_us;
    uint32_t fr_pulses, fr_bad;
} C;

static void s_ecg(uint32_t start, uint8_t f, const int16_t *s)
{
    (void)f; (void)s;
    if (start != C.next_ecg) C.ecg_gap = true;
    C.next_ecg = start + 10;
    C.ecg_blocks++;
}
static void s_ppg(uint32_t start, uint16_t led, uint8_t f, const int16_t *cf)
{
    (void)led; (void)f; (void)cf;
    if (start != C.next_ppg) C.ppg_gap = true;
    C.next_ppg = start + 2;
    C.ppg_blocks++;
}
static void s_beat(uint32_t i, uint16_t hr) { (void)i; (void)hr; C.beats++; }
static void s_pulse(uint32_t i, uint16_t b, uint16_t pat, uint16_t pi)
{
    (void)i; (void)b; (void)pi;
    C.pulses++;
    if (pat) { C.pat_sum += pat; C.pat_n++; }
}
static void s_contact(bool on) { if (on) C.contacts++; }
static void s_settled(bool on) { if (on) C.settles++; }
static const acq_sink_t sink = {s_ecg, s_ppg, NULL, NULL, s_beat, s_pulse, s_contact, s_settled};

static void boot(void)
{
    sim_reset();
    sim_set_violation_mode(SIM_VIOL_ABORT);
    memset(&C, 0, sizeof C);
    hal_init();
    rails_init();
    CHECK(mcp4728_init() == HAL_OK);
    acq_init(&sink);
}

/* Main-loop stand-in; also measures FAST_RESTORE pulse widths. */
static void run_until(uint64_t t_ms)
{
    bool fr = false;
    while (sim_now_ms() < t_ms) {
        acq_poll(hal_millis());
        bool now = sim_pin(PIN_FAST_RESTORE);
        if (now && !fr) C.fr_rise_us = sim_now_us();
        if (!now && fr) {
            uint64_t w = sim_now_us() - C.fr_rise_us;
            C.fr_pulses++;
            if (w < 20000 || w > 50000) C.fr_bad++;
        }
        fr = now;
        hal_wdt_kick();
        hal_idle();
    }
}

static acq_cfg_t cfg(bool ecg, bool ppg)
{
    acq_cfg_t c = {ecg, ppg, false, RLD_BOTH, NOTCH_AUTO, 15};
    return c;
}

TEST(vmid_fault)
{
    boot();
    sim_ecg_set_vmid_error_mv(40);
    acq_cfg_t c = cfg(true, false);
    CHECK(acq_start(&c));
    run_until(1000);
    CHECK(acq_status()->phase == ACQ_FAILED);
    CHECK(acq_status()->err == ACQ_ERR_VMID);
    CHECK(!sim_pin(PIN_ANA_EN) && !sim_pin(PIN_MEASURE_REQ));
}

TEST(ecg_session)
{
    boot();
    testsrc_cfg_t tc = testsrc_defaults();
    testsrc_install(&tc);
    acq_cfg_t c = cfg(true, false);
    CHECK(acq_start(&c));
    run_until(2000);
    CHECK(acq_status()->phase == ACQ_ON);
    CHECK(!acq_status()->contact);
    CHECK(sim_rail(SIM_RAIL_BODY_EN));
    sim_set_thumbs(true);
    run_until(4500);
    CHECK(acq_status()->contact);
    CHECK(!acq_status()->settled);
    run_until(20000);
    CHECK(acq_status()->settled);
    CHECK_NEAR(acq_status()->hr_bpm, tc.hr_bpm, 2);
    CHECK(C.fr_pulses >= 2); /* start-up pulse + contact pulse */
    CHECK_EQ(C.fr_bad, 0);
    CHECK(!C.ecg_gap);
    CHECK_EQ(hal_ecg_overruns(), 0);
    CHECK(sim_now_ms() == 20000);
    uint32_t idx0 = acq_status()->ecg_samples;
    /* a 150 ms stall loses samples; the timeline must stay intact */
    hal_delay_ms(150);
    run_until(21000);
    CHECK(hal_ecg_overruns() > 0);
    uint32_t idx = acq_status()->ecg_samples;
    CHECK_NEAR((double)(idx - idx0), 500.0, 1); /* 1 s of 500 Hz despite the loss */
    CHECK(!C.ecg_gap);
    sim_set_thumbs(false);
    run_until(22000);
    CHECK(!acq_status()->contact);
    acq_stop();
    CHECK(acq_summary()->ecg_lost > 0);
    CHECK(acq_summary()->hrv.beats >= 15);
    CHECK_NEAR(acq_summary()->hrv.hr_avg, tc.hr_bpm, 3);
    CHECK(!sim_pin(PIN_ANA_EN) && !sim_rail(SIM_RAIL_BODY_EN));
}

TEST(ppg_session)
{
    boot();
    testsrc_cfg_t tc = testsrc_defaults();
    tc.refl_ua_per_ma = 0.6; /* too bright at 5 mA: AGC must step down */
    testsrc_install(&tc);
    sim_set_finger(true);
    acq_cfg_t c = cfg(false, true);
    CHECK(acq_start(&c));
    run_until(30000);
    const acq_status_t *s = acq_status();
    CHECK(s->phase == ACQ_ON);
    CHECK(s->finger && s->settled);
    CHECK(s->led_code < dsp_led_max_code(LED_START_MA));
    double tia = sim_ppg_tia_v();
    (void)tia;
    CHECK_NEAR(s->pulse_bpm, tc.hr_bpm, 2);
    CHECK(!C.ppg_gap);
    CHECK_EQ(sim_facts()->ppg_offset_in_sample_window, 0);
    CHECK_EQ(sim_facts()->ppg_vled_early_starts, 0);
    CHECK_EQ(hal_ppg_overruns(), 0);
    acq_stop();
    CHECK(acq_summary()->pulses > 20);
    CHECK_NEAR(acq_summary()->pulse_avg, tc.hr_bpm, 2);
    CHECK(sim_led_now_ma() == 0.0 && !sim_pin(PIN_VLED_EN));
}

TEST(both_pat)
{
    boot();
    testsrc_cfg_t tc = testsrc_defaults();
    tc.pat_s = 0.230;
    testsrc_install(&tc);
    sim_set_finger(true);
    sim_set_thumbs(true);
    acq_cfg_t c = cfg(true, true);
    CHECK(acq_start(&c));
    run_until(40000);
    CHECK(acq_status()->settled);
    CHECK(C.pat_n > 10);
    double truth = testsrc_true_pat_ms(5, 40);
    double pat = (double)C.pat_sum / C.pat_n;
    printf("  PAT %.1f ms, truth %.1f ms\n", pat, truth);
    CHECK_NEAR(pat, truth, 10);
    CHECK_NEAR(acq_status()->pulse_bpm, tc.hr_bpm, 2);
    CHECK_NEAR(acq_status()->hr_bpm, tc.hr_bpm, 2);
    acq_stop();
    CHECK(acq_summary()->pat_n > 10);
}

int main(void)
{
    RUN(vmid_fault);
    RUN(ecg_session);
    RUN(ppg_session);
    RUN(both_pat);
    return TEST_RESULT();
}
