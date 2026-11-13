#include "cw2015.h"

#include "drv_i2c.h"

#define ADDR          I2C_ADDR_CW2015
#define REG_VERSION   0x00u
#define REG_VCELL     0x02u
#define REG_SOC       0x04u
#define REG_RRT_ALRT  0x06u
#define REG_CONFIG    0x08u
#define REG_MODE      0x0Au
#define MODE_AWAKE    0x00u
#define MODE_SLEEP    0xC0u

static uint8_t s_version;

static hal_status_t rd(uint8_t reg, uint8_t *dst, size_t n)
{
    return drv_i2c_xfer(ADDR, &reg, 1, dst, n);
}

static hal_status_t wr(uint8_t reg, uint8_t val)
{
    uint8_t w[2] = {reg, val};
    return drv_i2c_xfer(ADDR, w, sizeof w, NULL, 0);
}

hal_status_t cw2015_init(void)
{
    hal_status_t st = rd(REG_VERSION, &s_version, 1);
    if (st != HAL_OK) {
        return st;
    }
    st = wr(REG_MODE, MODE_AWAKE);
    if (st != HAL_OK) {
        return st;
    }

    /* HOOK: battery profile. When a profile for the chosen cell is available:
     * if CONFIG.UFG (bit 1) is clear or the stored profile differs, write the
     * 64-byte profile to 0x10-0x4F, set UFG, and restart the gauge (MODE
     * 0x0A sleep then wake), as in the Linux cw2015_battery driver.
     * NOT IMPLEMENTED in this build: nothing is written to 0x10-0x4F and UFG
     * is never set by this driver. */

    uint8_t cfg;
    st = rd(REG_CONFIG, &cfg, 1);
    if (st != HAL_OK) {
        return st;
    }
    uint8_t want = (uint8_t)((cfg & 0x07u) | ((BATT_WARN_PCT & 0x1Fu) << 3));
    if (want != cfg) {
        st = wr(REG_CONFIG, want);
    }
    return st;
}

uint8_t cw2015_version(void) { return s_version; }
bool cw2015_version_ok(void) { return s_version == CW2015_VERSION_EXPECTED; }

hal_status_t cw2015_read(uint16_t *mv, uint8_t *soc_pct)
{
    uint8_t b[4]; /* VCELL H, L, SOC H, SOC L */
    hal_status_t st = rd(REG_VCELL, b, sizeof b);
    if (st != HAL_OK) {
        return st;
    }
    uint32_t raw = (((uint32_t)b[0] << 8) | b[1]) & 0x3FFFu;
    *mv = (uint16_t)(raw * 305u / 1000u);
    *soc_pct = b[2];
    return b[2] > 100u ? HAL_ERR_PARAM : HAL_OK;
}

hal_status_t cw2015_clear_alert(void)
{
    uint8_t v;
    hal_status_t st = rd(REG_RRT_ALRT, &v, 1);
    if (st != HAL_OK) {
        return st;
    }
    return wr(REG_RRT_ALRT, (uint8_t)(v & 0x7Fu));
}

hal_status_t cw2015_sleep(void) { return wr(REG_MODE, MODE_SLEEP); }

static const struct {
    uint16_t mv;
    uint8_t pct;
} k_curve[] = {
    {3300, 0},  {3500, 3},  {3600, 8},  {3700, 18}, {3750, 28}, {3800, 40},
    {3850, 52}, {3900, 62}, {4000, 78}, {4100, 90}, {4200, 100},
};
#define CURVE_N (sizeof k_curve / sizeof k_curve[0])

uint8_t batt_pct_from_mv(uint16_t mv)
{
    if (mv <= k_curve[0].mv) {
        return 0;
    }
    if (mv >= k_curve[CURVE_N - 1].mv) {
        return 100;
    }
    for (unsigned i = 1; i < CURVE_N; i++) {
        if (mv <= k_curve[i].mv) {
            uint32_t dv = (uint32_t)(mv - k_curve[i - 1].mv);
            uint32_t span = (uint32_t)(k_curve[i].mv - k_curve[i - 1].mv);
            uint32_t dp = (uint32_t)(k_curve[i].pct - k_curve[i - 1].pct);
            return (uint8_t)(k_curve[i - 1].pct + (dv * dp + span / 2u) / span);
        }
    }
    return 100;
}

uint8_t batt_pct_combine(uint16_t mv, uint8_t gauge_soc)
{
    uint8_t v = batt_pct_from_mv(mv);
    if (gauge_soc > 100u) {
        return v;
    }
    if (gauge_soc == 0u && mv > 3700u) {
        return v;
    }
    int diff = (int)gauge_soc - (int)v;
    if (diff > 30 || diff < -30) {
        return v;
    }
    return gauge_soc;
}
