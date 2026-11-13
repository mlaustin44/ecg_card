/* System-scenario harness: the real application (app + acq + drivers + dsp
 * + ui) on the host simulator in ABORT mode, so any rail-rule violation
 * aborts the test binary. Adds its own per-step checks (see scen_stats_t). */
#ifndef SCEN_H
#define SCEN_H

#include <stdbool.h>
#include <stdint.h>

#include "app.h"
#include "sim.h"
#include "test.h"
#include "testsrc.h"

typedef struct {
    uint64_t steps;
    uint64_t max_step_us;      /* longest app_step() while awake (incl. its hal_idle) */
    uint64_t max_step_acq_us;  /* ... while acquisition is active */
    uint32_t body_en_outside_session;  /* BODY_EN high without an ECG acquisition */
    uint32_t measure_req_on_usb;       /* MEASURE_REQ high with USB present */
    uint32_t analog_outside;           /* analog enables high with nothing that needs them */
    double max_led_ma;                 /* largest LED current seen lit */
    uint32_t violations_seen;
} scen_stats_t;

extern scen_stats_t scen;

/* Fresh world (sim_reset, ABORT mode) then app_init(). keep_nv: keep the
 * flash content of the previous test. */
void scen_boot(bool keep_nv);
/* MCU reset (sim_reboot) then app_init(); the world keeps its state. */
void scen_reboot(void);

/* Run the main loop until absolute sim time t_ms (or sim_should_stop()). */
void scen_run_until(uint64_t t_ms);
void scen_run_ms(uint64_t ms);
/* Run until cond() or timeout; returns cond(). */
bool scen_run_cond(bool (*cond)(void), uint64_t timeout_ms);

/* Press a button now for hold_ms, run until 60 ms after the release. */
void scen_press(int button, uint64_t hold_ms);
void scen_tap(int button);
/* Both buttons together for hold_ms. */
void scen_press_both(uint64_t hold_ms);

/* Model of the last frame drawn, and helpers on it. */
const ui_model_t *scen_m(void);
ui_screen_t scen_screen(void);
ui_meas_state_t scen_live_state(void);

/* Simulated panel: number of lit pixels (0 = blank / off). */
int scen_lit_pixels(void);
/* build/app_shots/<name>.png at 4x. */
void scen_shot(const char *name);

/* Every enable / guarded pin low, LEDs dark, OLED may be on. */
bool scen_analog_all_off(void);

/* ---- C-side decode of the captured stream (sim_stream_data) ---- */
typedef struct {
    uint32_t frames, bad_cobs, bad_crc, bad_len, seq_gaps;
    uint32_t ecg_blocks, ppg_blocks, ecg_index_gaps, ppg_index_gaps, runs;
    uint32_t r_peaks, pulses, leads, states, buttons;
    uint32_t pat_n;
    double pat_sum_ms;
    uint32_t hr_n;
    double hr_sum;
    uint32_t ecg_settling_blocks, ecg_leadoff_blocks, ppg_nofinger_blocks;
} scen_decoded_t;
scen_decoded_t scen_decode_stream(void);

#endif
