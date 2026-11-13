#include "diag.h"

#include "applog.h"
#include "board.h"
#include "hal.h"

typedef struct {
    char *b, *end;
} line_t;

static line_t L(char *row) { line_t l = {row, row + UI_DIAG_COLS}; return l; }
static void s(line_t *l, const char *t) { l->b = fmt_s(l->b, l->end, t); }
static void u(line_t *l, uint32_t v) { l->b = fmt_u(l->b, l->end, v); }
static void x(line_t *l, uint32_t v, unsigned n) { l->b = fmt_x(l->b, l->end, v, n); }
static void mv(line_t *l, uint16_t code)
{
    u(l, ((uint32_t)code * ADC_UV_PER_CODE_X10 + 5000u) / 10000u);
    s(l, "mV");
}

static const char *reset_name(uint8_t c)
{
    switch (c) {
    case HAL_RESET_POWER_ON: return "power-on";
    case HAL_RESET_PIN: return "pin";
    case HAL_RESET_WATCHDOG: return "WATCHDOG";
    case HAL_RESET_SOFTWARE: return "software";
    case HAL_RESET_BROWNOUT: return "brown-out";
    default: return "other";
    }
}

void diag_format(uint8_t page, const diag_info_t *d, char out[UI_DIAG_LINES][UI_DIAG_COLS + 1])
{
    for (unsigned i = 0; i < UI_DIAG_LINES; i++)
        out[i][0] = '\0';
    line_t l;
    switch (page) {
    case 0:
        l = L(out[0]); s(&l, "PINS / SYSTEM");
        l = L(out[1]); s(&l, "CHG_PG "); u(&l, d->chg_pg); s(&l, " STAT1 "); u(&l, d->chg_stat1);
        l = L(out[2]); s(&l, "BTN1 "); s(&l, d->btn1_raw ? "down" : "up"); s(&l, " BTN2 ");
        s(&l, d->btn2_raw ? "down" : "up"); if (d->btn_fault) s(&l, " !");
        l = L(out[3]); s(&l, "Charger "); s(&l, d->charger);
        l = L(out[4]); s(&l, "Reset "); s(&l, reset_name(d->reset_cause));
        l = L(out[5]); s(&l, "Up "); u(&l, d->uptime_s); s(&l, "s loop "); u(&l, d->loop_max_ms);
        s(&l, "ms");
        break;
    case DIAG_PAGE_ANALOG:
        l = L(out[0]); s(&l, "ANALOG  ANA_EN "); s(&l, d->analog_on ? "on" : "off");
        if (d->analog_on && d->analog_ms < ECG_VMID_SETTLE_MS) {
            l = L(out[1]); s(&l, "settling...");
            break;
        }
        l = L(out[1]); s(&l, "VMID "); u(&l, d->vmid); s(&l, " "); mv(&l, d->vmid);
        l = L(out[2]); s(&l, "  (1650mV +-"); u(&l, ECG_VMID_TOL_MV); s(&l, ")");
        l = L(out[3]); s(&l, "ECG  "); u(&l, d->ecg); s(&l, " "); mv(&l, d->ecg);
        l = L(out[4]); s(&l, "TIA  "); u(&l, d->tia); s(&l, " "); mv(&l, d->tia);
        l = L(out[5]); s(&l, "PPGHI "); u(&l, d->ppg_hi); s(&l, " "); mv(&l, d->ppg_hi);
        break;
    case 2:
        l = L(out[0]); s(&l, "I2C 60:"); s(&l, d->ack60 ? "ACK" : "--"); s(&l, " 62:");
        s(&l, d->ack62 ? "ACK" : "--");
        l = L(out[1]); s(&l, "CW2015 ver 0x"); x(&l, d->cw_version, 2);
        s(&l, d->gauge_ok ? (d->cw_version == 0x73u ? " ok" : " ?") : " FAIL");
        l = L(out[2]); s(&l, "VCELL "); u(&l, d->vcell_mv); s(&l, "mV");
        l = L(out[3]); s(&l, "SOC "); u(&l, d->soc_gauge); s(&l, "% use ");
        if (d->batt_valid) { u(&l, d->batt_pct); s(&l, "%"); } else s(&l, "--");
        l = L(out[4]); s(&l, "MCP4728 ");
        s(&l, !d->mcp_ok ? "FAIL" : d->mcp_eeprom_written ? "EEPROM set" : "EEPROM ok");
        l = L(out[5]); s(&l, "I2C errors "); u(&l, d->i2c_errors);
        break;
    case 3:
        l = L(out[0]); s(&l, d->have_session ? "LAST SESSION" : "LAST SESSION: none");
        l = L(out[1]); s(&l, "lost ECG "); u(&l, d->ecg_lost); s(&l, " PPG "); u(&l, d->ppg_lost);
        l = L(out[2]); s(&l, "drop "); u(&l, d->stream_drops); s(&l, "/"); u(&l, d->stream_frames);
        l = L(out[3]); s(&l, "loop "); u(&l, d->loop_acq_max_ms); s(&l, "ms i2c err "); u(&l, d->acq_i2c_errors);
        l = L(out[4]); s(&l, "LED "); u(&l, d->led_code); s(&l, " DAC12 "); u(&l, d->dac12);
        l = L(out[5]); s(&l, "mid "); u(&l, d->mid_code); s(&l, d->gain_hi ? " 1098x " : " 558x ");
        u(&l, d->notch_hz); s(&l, "Hz");
        break;
    default:
        l = L(out[0]); s(&l, "FIRMWARE");
        l = L(out[1]); s(&l, "ECG Card "); s(&l, d->fw_version);
        l = L(out[2]); s(&l, __DATE__);
        l = L(out[3]); s(&l, __TIME__); s(&l, " MSPM0G3507");
        l = L(out[4]); s(&l, "Radio: off (no BLE)");
        l = L(out[5]); s(&l, "NV ");
        s(&l, d->settings_status == 0 ? "ok" : d->settings_status == 1 ? "blank" : "corrupt");
        s(&l, " writes "); u(&l, d->settings_writes);
        break;
    }
}
