/* Diagnostics pages (hidden: both buttons long on About). Text only,
 * UI_DIAG_LINES x UI_DIAG_COLS. The app gathers the facts (diag_info_t),
 * this module formats them.
 *   1 pins     CHG_PG, CHG_STAT1, buttons, charger state, reset cause, uptime
 *   2 analog   ANA_EN is on while this page is shown: VMID, ECG, TIA, PPG_HI
 *   3 I2C      probes 0x60 / 0x62, CW2015 version, VCELL, SOC, MCP4728 EEPROM
 *   4 acq      last session: overruns, stream drops, loop latency, I2C errors,
 *              LED code, DAC12, mid code, gain, notch
 *   5 firmware version, build, settings
 *   6 radio    link state, HELLO (radio firmware, address), connection (MTU,
 *              interval, subscriptions), UART bytes, CRC / bad / gap / unknown
 *              counts, dropped frames. BUTTON1 toggles program mode: RADIO_EN
 *              held on for flashing the CH585 through its own debug port, the
 *              MCU silent, no timeouts; leaving the page ends it. */
#ifndef APP_DIAG_H
#define APP_DIAG_H

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"
#include "radio.h"
#include "ui_model.h"

#define DIAG_PAGES 6u
#define DIAG_PAGE_ANALOG 1u
#define DIAG_PAGE_RADIO 5u

typedef struct {
    /* page 1 */
    bool chg_pg, chg_stat1, btn1_raw, btn2_raw, btn_fault;
    const char *charger;
    uint8_t reset_cause; /* hal_reset_cause_t */
    uint32_t uptime_s;
    /* page 2 */
    bool analog_on;
    uint32_t analog_ms;  /* since ANA_EN rose */
    uint16_t vmid, ecg, tia, ppg_hi; /* one-shot codes */
    /* page 3 */
    bool ack60, ack62;
    bool gauge_ok;
    uint8_t cw_version;
    uint16_t vcell_mv;
    uint8_t soc_gauge, batt_pct;
    bool batt_valid;
    bool mcp_ok, mcp_eeprom_written;
    uint32_t i2c_errors, i2c_recoveries_hint;
    /* page 4 */
    bool have_session;
    uint32_t ecg_lost, ppg_lost, stream_drops, stream_frames;
    uint32_t loop_max_ms, loop_acq_max_ms;
    uint16_t led_code, dac12, mid_code;
    bool gain_hi;
    uint8_t notch_hz;
    uint32_t acq_i2c_errors;
    /* page 5 */
    const char *fw_version;
    uint8_t settings_status; /* settings_load_t */
    uint32_t settings_writes;
    /* page 6 */
    const radio_info_t *radio; /* NULL: not gathered yet */
    bool radio_program;
    uint32_t radio_tx_frames, radio_tx_drops;
    hal_uart_stats_t uart;
} diag_info_t;

void diag_format(uint8_t page, const diag_info_t *d, char out[UI_DIAG_LINES][UI_DIAG_COLS + 1]);

#endif
