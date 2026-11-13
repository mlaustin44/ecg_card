/* Internal interfaces between the simulator's modules. Not for tests. */
#ifndef SIM_PRIV_H
#define SIM_PRIV_H

#include "sim.h"

/* ---- core (sim_core.c) ---- */
void sim_viol(sim_viol_t id, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sim_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void sim_advance_us(uint64_t dt_us);   /* may fire scripted events */
void sim_service(void);                /* fire everything due at "now" */
void sim_input_set(board_pin_t pin, bool level); /* world-driven input edge */
bool sim_acq_running_any(void);
sim_facts_t *sim_facts_raw(void);      /* sim_facts() without bringing trackers up to date */
void sim_core_set_led_blank(bool level);
void sim_core_set_oled_attached(bool attached);
bool sim_body_en(void);
uint64_t sim_vled_rise_us(void);
uint64_t sim_rng_next(void);
uint16_t sim_battery_mv_get(void);
int sim_soc_override_get(void);
bool sim_thumbs(void);
bool sim_finger(void);
double sim_gauss(void);                /* N(0,1) */

/* ---- analog (sim_analog.c) ---- */
void an_reset(void);                   /* power-on */
void an_mcu_reset(void);               /* MCU reset */
uint64_t an_next_event_us(void);       /* UINT64_MAX if none */
void an_run_events(uint64_t t_us);
void an_sync(void);                    /* bring continuous models up to now */
void an_pin_changed(board_pin_t p, bool old_level, bool new_level);
void an_world_changed(void);           /* thumbs / finger */
bool an_ppg_running(void);
bool an_ppg_analog_on(void);
unsigned an_running(void);

/* ---- i2c / devices (sim_i2c.c) ---- */
void i2c_reset(void);
void mcp_a_state(bool *active, double *vout_mv);
void mcp_vref_track(void);             /* after DAC register or VLED_EN changes */
void cw_battery_changed(void);
void cw_power_on(void);

/* ---- oled (sim_oled.c) ---- */
void oled_reset(void);
void oled_mcu_reset(void);
void oled_pin_changed(board_pin_t p, bool old_level, bool new_level);
void oled_progress(void);              /* consume async bytes due by now */
bool oled_dma_active(void);

#endif
