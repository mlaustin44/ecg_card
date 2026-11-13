/* I2C0 transport, MCP4728 and CW2015 register-level models. */
#include <math.h>
#include <string.h>

#include "sim_priv.h"

/* ------------------------------------------------------------------------ */
/* MCP4728                                                                   */

typedef struct {
    uint16_t code;
    uint8_t vref; /* 1 = internal 2.048 V */
    uint8_t pd;
    uint8_t gain; /* 1 = x2 */
} mcp_ch_t;

static struct {
    mcp_ch_t in[4], ee[4];
    uint64_t busy_until_us;
    bool a_fw_written; /* firmware has written channel A since power-up */
} M;

static struct {
    uint8_t reg[0x50];
    uint8_t ptr;
    bool asleep;
    uint16_t frozen_vcell;
    uint8_t frozen_soc, frozen_frac;
    bool alert_flag, prev_below;
    uint32_t profile_txns;
    uint8_t profile[64];
} C;

static struct {
    unsigned fail_n;
    hal_status_t fail_status;
    bool stuck;
    bool absent_mcp, absent_cw; /* device not fitted / dead: NACKs its address */
    sim_i2c_txn_t log[1024];
    size_t log_n;
} I;

static void mcp_factory(void)
{
    for (int i = 0; i < 4; i++) M.ee[i] = (mcp_ch_t){0x800, 0, 0, 0};
}

void mcp_a_state(bool *active, double *vout_mv)
{
    *active = M.in[0].pd == 0 && M.a_fw_written;
    *vout_mv = sim_mcp_vout_mv(0);
}

unsigned sim_mcp_code(int ch) { return M.in[ch & 3].code; }
bool sim_mcp_vref_internal(int ch) { return M.in[ch & 3].vref; }
bool sim_mcp_gain2(int ch) { return M.in[ch & 3].gain; }
unsigned sim_mcp_pd(int ch) { return M.in[ch & 3].pd; }
double sim_mcp_vout_mv(int ch)
{
    mcp_ch_t *c = &M.in[ch & 3];
    if (c->pd) return 0.0;
    double v;
    if (c->vref) {
        v = (double)c->code / 4096.0 * 2048.0 * (c->gain ? 2.0 : 1.0);
        if (v > 3300.0) v = 3300.0;
    } else {
        v = (double)c->code / 4096.0 * 3300.0;
    }
    return v;
}
void sim_mcp_eeprom_get(int ch, unsigned *code, bool *vref, unsigned *pd, bool *gain2)
{
    mcp_ch_t *c = &M.ee[ch & 3];
    if (code) *code = c->code;
    if (vref) *vref = c->vref;
    if (pd) *pd = c->pd;
    if (gain2) *gain2 = c->gain;
}
void sim_mcp_eeprom_set(int ch, unsigned code, bool vref, unsigned pd, bool gain2)
{
    M.ee[ch & 3] = (mcp_ch_t){(uint16_t)(code & 0xFFF), vref, (uint8_t)(pd & 3), gain2};
}
bool sim_mcp_busy(void) { return sim_now_us() < M.busy_until_us; }
void sim_mcp_power_cycle(void)
{
    for (int i = 0; i < 4; i++) M.in[i] = M.ee[i];
    M.busy_until_us = 0;
    M.a_fw_written = false;
}
double sim_led_set_ma(void) { return sim_mcp_vout_mv(0) / 1000.0 * (10.0 / 164.0) / 8.2 * 1000.0; }
double sim_led_now_ma(void)
{
    if (sim_led_blank() || !sim_pin(PIN_VLED_EN) || !sim_pin(PIN_ANA_EN)) return 0.0;
    return sim_led_set_ma();
}

static void mcp_eeprom_write(int ch)
{
    M.ee[ch] = M.in[ch];
    ((sim_facts_t *)sim_facts())->mcp_eeprom_writes++;
    M.busy_until_us = sim_now_us() + 50000;
}

/* Returns false if the transaction was malformed (still ACKed). */
static void mcp_write(const uint8_t *w, size_t n)
{
    size_t i = 0;
    M.a_fw_written = true;
    while (i < n) {
        uint8_t b = w[i];
        if ((b >> 6) == 0) { /* fast write: 0 0 PD1 PD0 D11..D8, D7..D0 per channel A.. */
            int ch = 0;
            while (i + 1 < n && ch < 4 && (w[i] >> 6) == 0) {
                M.in[ch].pd = (w[i] >> 4) & 3;
                M.in[ch].code = (uint16_t)(((w[i] & 0x0F) << 8) | w[i + 1]);
                i += 2;
                ch++;
            }
            if (i < n && ch < 4 && (w[i] >> 6) == 0) {
                sim_viol(SIM_V_I2C_PROTOCOL, "MCP4728 fast write truncated");
                return;
            }
            continue;
        }
        switch (b >> 5) {
        case 2: /* 0b010: write DAC input register (+EEPROM) */ {
            unsigned wsel = (b >> 3) & 3;
            int ch = (b >> 1) & 3;
            if (wsel == 1) {
                sim_viol(SIM_V_I2C_PROTOCOL, "MCP4728 invalid write command 0x%02X", b);
                return;
            }
            bool eeprom = wsel >= 2;
            if (eeprom && sim_mcp_busy()) {
                ((sim_facts_t *)sim_facts())->mcp_writes_ignored_busy++;
                sim_viol(SIM_V_I2C_PROTOCOL,
                         "MCP4728 EEPROM write command 0x%02X sent while BSY (ignored)", b);
                return;
            }
            i++;
            int count = wsel == 2 ? 4 - ch : 1;
            if (wsel == 0) count = 1;
            int done = 0;
            while (done < count && i + 1 < n) {
                mcp_ch_t *c = &M.in[ch + done];
                c->vref = (w[i] >> 7) & 1;
                c->pd = (w[i] >> 5) & 3;
                c->gain = (w[i] >> 4) & 1;
                c->code = (uint16_t)(((w[i] & 0x0F) << 8) | w[i + 1]);
                if (eeprom) mcp_eeprom_write(ch + done);
                i += 2;
                done++;
            }
            if (done == 0) {
                sim_viol(SIM_V_I2C_PROTOCOL, "MCP4728 write command 0x%02X without data", b);
                return;
            }
            if (wsel != 2 && done < count) {
                sim_viol(SIM_V_I2C_PROTOCOL, "MCP4728 write truncated");
                return;
            }
            break;
        }
        case 4: /* 0b100 VREF select */
            for (int c = 0; c < 4; c++) M.in[c].vref = (b >> (3 - c)) & 1;
            i++;
            break;
        case 6: /* 0b110 gain select */
            for (int c = 0; c < 4; c++) M.in[c].gain = (b >> (3 - c)) & 1;
            i++;
            break;
        case 5: /* 0b101 power-down select */
            if (i + 1 >= n) {
                sim_viol(SIM_V_I2C_PROTOCOL, "MCP4728 PD command truncated");
                return;
            }
            M.in[0].pd = (b >> 2) & 3;
            M.in[1].pd = b & 3;
            M.in[2].pd = (w[i + 1] >> 6) & 3;
            M.in[3].pd = (w[i + 1] >> 4) & 3;
            i += 2;
            break;
        default:
            sim_viol(SIM_V_I2C_PROTOCOL, "MCP4728 unknown command byte 0x%02X", b);
            return;
        }
    }
}

static void mcp_read(uint8_t *r, size_t rn)
{
    uint8_t img[24];
    unsigned ready = sim_mcp_busy() ? 0u : 1u;
    for (int c = 0; c < 4; c++) {
        uint8_t *p = &img[c * 6];
        uint8_t hdr = (uint8_t)((ready << 7) | (1u << 6) | ((unsigned)c << 4));
        p[0] = hdr;
        p[1] = (uint8_t)((M.in[c].vref << 7) | (M.in[c].pd << 5) | (M.in[c].gain << 4) |
                         (M.in[c].code >> 8));
        p[2] = (uint8_t)M.in[c].code;
        p[3] = hdr;
        p[4] = (uint8_t)((M.ee[c].vref << 7) | (M.ee[c].pd << 5) | (M.ee[c].gain << 4) |
                         (M.ee[c].code >> 8));
        p[5] = (uint8_t)M.ee[c].code;
    }
    for (size_t i = 0; i < rn; i++) r[i] = img[i % 24];
}

/* ------------------------------------------------------------------------ */
/* CW2015                                                                    */

static const struct {
    uint16_t mv;
    uint16_t soc_x10;
} soc_table[] = {{3300, 0},  {3500, 40},  {3600, 100}, {3700, 250}, {3800, 500},
                 {3900, 650}, {4000, 800}, {4100, 920}, {4200, 1000}};

static double soc_pct(void)
{
    int ov = sim_soc_override_get();
    if (ov >= 0) return (double)ov;
    uint16_t mv = sim_battery_mv_get();
    size_t n = sizeof soc_table / sizeof soc_table[0];
    if (mv <= soc_table[0].mv) return 0.0;
    if (mv >= soc_table[n - 1].mv) return 100.0;
    for (size_t i = 1; i < n; i++)
        if (mv <= soc_table[i].mv) {
            double f = (double)(mv - soc_table[i - 1].mv) / (double)(soc_table[i].mv - soc_table[i - 1].mv);
            return ((double)soc_table[i - 1].soc_x10 +
                    f * (double)(soc_table[i].soc_x10 - soc_table[i - 1].soc_x10)) / 10.0;
        }
    return 100.0;
}

static void cw_snapshot(void)
{
    double s = soc_pct();
    unsigned x256 = (unsigned)(s * 256.0);
    if (x256 > 100u * 256u) x256 = 100u * 256u;
    C.frozen_soc = (uint8_t)(x256 >> 8);
    C.frozen_frac = (uint8_t)x256;
    unsigned vc = (unsigned)((double)sim_battery_mv_get() * 1000.0 / 305.0);
    if (vc > 0x3FFF) vc = 0x3FFF;
    C.frozen_vcell = (uint16_t)vc;
}

static void cw_eval(void)
{
    if (!C.asleep) {
        cw_snapshot();
        unsigned athd = C.reg[0x08] >> 3;
        bool below = athd > 0 && C.frozen_soc <= athd;
        if (below && !C.prev_below) C.alert_flag = true;
        C.prev_below = below;
    }
    sim_input_set(PIN_CW_ALRT, !C.alert_flag);
}

void cw_battery_changed(void) { cw_eval(); }

void cw_power_on(void)
{
    memset(&C, 0, sizeof C);
    C.reg[0x00] = 0x73;
    C.reg[0x0A] = 0xC0;
    C.asleep = true;
}

unsigned sim_cw_profile_writes(void) { return C.profile_txns; }
void sim_cw_profile(uint8_t out[64]) { memcpy(out, &C.reg[0x10], 64); }
bool sim_cw_ufg(void) { return (C.reg[0x08] & 0x02) != 0; }
unsigned sim_cw_athd(void) { return C.reg[0x08] >> 3; }
bool sim_cw_asleep(void) { return C.asleep; }
bool sim_cw_alert_flag(void) { return C.alert_flag; }
uint8_t sim_cw_reg(unsigned a) { return a < 0x50 ? C.reg[a] : 0; }

static uint8_t cw_reg_read(unsigned a)
{
    switch (a) {
    case 0x02: return (uint8_t)(C.frozen_vcell >> 8);
    case 0x03: return (uint8_t)C.frozen_vcell;
    case 0x04: return C.frozen_soc;
    case 0x05: return C.frozen_frac;
    case 0x06: return (uint8_t)((C.alert_flag ? 0x80 : 0) | 0x1F);
    case 0x07: return 0xFF;
    default: return a < 0x50 ? C.reg[a] : 0;
    }
}

static hal_status_t cw_write(const uint8_t *w, size_t n)
{
    if (n == 0) return HAL_OK;
    if (w[0] >= 0x50) return HAL_ERR_NACK;
    C.ptr = w[0];
    bool touched_profile = false;
    sim_facts_t *f = (sim_facts_t *)sim_facts();
    for (size_t i = 1; i < n; i++) {
        unsigned a = C.ptr;
        if (a >= 0x50) return HAL_ERR_NACK;
        uint8_t v = w[i];
        if (a <= 0x05 || a == 0x07) {
            sim_viol(SIM_V_I2C_PROTOCOL, "CW2015 write to read-only register 0x%02X", a);
        } else if (a == 0x06) {
            if (!(v & 0x80)) {
                C.alert_flag = false;
            }
        } else if (a == 0x08) {
            C.reg[a] = v;
            C.prev_below = false; /* threshold changed: re-arm */
        } else if (a == 0x0A) {
            C.reg[a] = v;
            f->cw_mode_writes++;
            bool sleep = (v & 0xC0) == 0xC0;
            if (C.asleep && !sleep) C.asleep = false;
            else if (!C.asleep && sleep) C.asleep = true;
            if (!sleep) C.prev_below = false;
        } else if (a >= 0x10) {
            C.reg[a] = v;
            touched_profile = true;
            f->cw_profile_bytes++;
        } else {
            C.reg[a] = v;
        }
        C.ptr = (uint8_t)(a + 1);
    }
    if (touched_profile) {
        C.profile_txns++;
        f->cw_profile_writes++;
    }
    cw_eval();
    return HAL_OK;
}

static void cw_read(uint8_t *r, size_t rn)
{
    for (size_t i = 0; i < rn; i++) {
        r[i] = cw_reg_read(C.ptr);
        if (C.ptr < 0x4F) C.ptr++;
    }
}

/* ------------------------------------------------------------------------ */
/* transport                                                                 */

void i2c_reset(void)
{
    memset(&M, 0, sizeof M);
    mcp_factory();
    sim_mcp_power_cycle();
    memset(&I, 0, sizeof I);
}

void sim_i2c_fail_next(unsigned n, hal_status_t st) { I.fail_n = n; I.fail_status = st; }
void sim_i2c_stick_bus(bool s) { I.stuck = s; }
void sim_i2c_set_present(uint8_t addr, bool present)
{
    if (addr == I2C_ADDR_MCP4728) I.absent_mcp = !present;
    else if (addr == I2C_ADDR_CW2015) I.absent_cw = !present;
    else sim_viol(SIM_V_API, "sim_i2c_set_present: no device at 0x%02X", addr);
}
bool sim_i2c_bus_stuck(void) { return I.stuck; }
size_t sim_i2c_log_count(void) { return I.log_n; }
const sim_i2c_txn_t *sim_i2c_log_get(size_t i) { return i < I.log_n ? &I.log[i] : NULL; }
void sim_i2c_log_clear(void) { I.log_n = 0; }

void hal_i2c_recover(void)
{
    ((sim_facts_t *)sim_facts())->i2c_recoveries++;
    I.stuck = false;
    sim_advance_us(100);
}

hal_status_t hal_i2c_xfer(uint8_t addr, const uint8_t *w, size_t wn, uint8_t *r, size_t rn)
{
    sim_facts_t *f = (sim_facts_t *)sim_facts();
    f->i2c_xfers++;
    sim_i2c_txn_t *lg = NULL;
    if (I.log_n < 1024) {
        lg = &I.log[I.log_n++];
        memset(lg, 0, sizeof *lg);
        lg->t_us = sim_now_us();
        lg->addr = addr;
        lg->wn = wn;
        lg->rn = rn;
        if (w && wn) memcpy(lg->w, w, wn < 32 ? wn : 32);
    }
#define DONE(st)                       \
    do {                               \
        hal_status_t st_ = (st);       \
        if (lg) lg->status = st_;      \
        return st_;                    \
    } while (0)

    if (addr > 0x7F || (wn && !w) || (rn && !r)) DONE(HAL_ERR_PARAM);
    if (I.stuck) {
        sim_advance_us(10000);
        DONE(HAL_ERR_BUS);
    }
    if (I.fail_n) {
        I.fail_n--;
        f->i2c_faults_injected++;
        sim_advance_us(I.fail_status == HAL_ERR_NACK ? 30 : 10000);
        DONE(I.fail_status);
    }
    /* 400 kHz: 2.5 us per bit, 9 bits per byte */
    uint64_t bits = 2 + 9 + (uint64_t)wn * 9 + (rn ? 9 + (uint64_t)rn * 9 : 0);
    uint64_t dur = bits * 5 / 2;

    if (addr == 0x00) { /* general call: ACK and ignore */
        sim_advance_us(dur);
        DONE(HAL_OK);
    }
    if ((addr != I2C_ADDR_MCP4728 && addr != I2C_ADDR_CW2015) ||
        (addr == I2C_ADDR_MCP4728 && I.absent_mcp) || (addr == I2C_ADDR_CW2015 && I.absent_cw)) {
        sim_advance_us(30);
        DONE(HAL_ERR_NACK);
    }
    sim_advance_us(dur); /* scripted events may fire; device effects land at the end */
    if (addr == I2C_ADDR_MCP4728) {
        if (wn) mcp_write(w, wn);
        if (rn) mcp_read(r, rn);
    } else {
        hal_status_t st = HAL_OK;
        if (wn) st = cw_write(w, wn);
        if (st != HAL_OK) DONE(st);
        if (rn) cw_read(r, rn);
    }
    DONE(HAL_OK);
#undef DONE
}
