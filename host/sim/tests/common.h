/* Shared helpers for simulator self-tests. */
#ifndef SIM_TEST_COMMON_H
#define SIM_TEST_COMMON_H
#include "sim.h"
#include "test.h"

static inline void fresh(void)
{
    sim_reset();
    sim_set_violation_mode(SIM_VIOL_COLLECT);
    hal_init();
}

static inline void high(board_pin_t p) { hal_pin_write(p, true); }
static inline void low(board_pin_t p) { hal_pin_write(p, false); }

/* ECG up as the brief's start sequence does it (thumbs/electrodes not included). */
static inline void ecg_rails_up(void)
{
    high(PIN_ANA_EN);
    high(PIN_BUCK_PS_SYNC);
    hal_delay_ms(300);
}
/* Stop sequence. */
static inline void ecg_rails_down(void)
{
    low(PIN_MEASURE_REQ);
    low(PIN_FAST_RESTORE);
    low(PIN_GAIN_HI);
    low(PIN_RLD_L_EN);
    low(PIN_RLD_R_EN);
    if (hal_acq_running()) hal_acq_stop();
    low(PIN_BUCK_PS_SYNC);
    low(PIN_ANA_EN);
}
#endif
