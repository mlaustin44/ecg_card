#include "mcp4728.h"

#include "drv_i2c.h"

#define ADDR           I2C_ADDR_MCP4728
#define BSY_TIMEOUT_MS 100u
#define READ_LEN       24u /* 4 x (3 bytes input register + 3 bytes EEPROM) */

/* Reference selection. DS22187E 4.10 / Register 5-3 note: "the internal
 * voltage reference circuit stays turned-on if it is selected as reference by
 * at least one channel", powered down or not (IPD_INT 45 uA typ vs IPD_EXT
 * 40 nA). The chip is on the always-on +3V3 rail, so every powered-down
 * channel selects VDD (VREF=0); only the write that turns channel A ON selects
 * the internal 2.048 V reference. */
#define VREF_VDD 0u
#define VREF_INT 1u
#define PD_1K    1u /* PD1:PD0 = 01: 1 kOhm to ground */

static bool s_led_on;
static bool s_eeprom_written;

/* Multi-Write block: 0100 0 ch UDAC=0 | VREF PD1 PD0 G D11..8 | D7..0 */
static void put_block(uint8_t *p, unsigned ch, unsigned vref, unsigned pd,
                      uint16_t code)
{
    p[0] = (uint8_t)(0x40u | (ch << 1));
    p[1] = (uint8_t)((vref << 7) | (pd << 5) | ((code >> 8) & 0x0Fu)); /* G=0 */
    p[2] = (uint8_t)(code & 0xFFu);
}

hal_status_t mcp4728_power_down_all(void)
{
    uint8_t w[12];
    for (unsigned ch = 0; ch < 4; ch++) {
        put_block(&w[ch * 3], ch, VREF_VDD, PD_1K, 0);
    }
    hal_status_t st = drv_i2c_xfer(ADDR, w, sizeof w, NULL, 0);
    if (st == HAL_OK) {
        s_led_on = false;
    }
    return st;
}

static hal_status_t read_back(uint8_t *rb)
{
    return drv_i2c_xfer(ADDR, NULL, 0, rb, READ_LEN);
}

/* Every channel's EEPROM copy must be {VREF=VDD, PD=01, gain x1, code 0}. A
 * part programmed by an earlier build (channel A with the internal reference)
 * fails this check and is re-programmed once. */
static bool eeprom_ok(const uint8_t *rb)
{
    for (unsigned ch = 0; ch < 4; ch++) {
        const uint8_t *e = &rb[ch * 6 + 3]; /* EEPROM copy */
        unsigned vref = e[1] >> 7;
        unsigned pd = (e[1] >> 5) & 3u;
        unsigned gx = (e[1] >> 4) & 1u;
        unsigned code = ((unsigned)(e[1] & 0x0Fu) << 8) | e[2];
        if (vref != VREF_VDD || pd != PD_1K || gx != 0u || code != 0u) {
            return false;
        }
    }
    return true;
}

hal_status_t mcp4728_init(void)
{
    uint8_t rb[READ_LEN];
    s_eeprom_written = false;
    /* Pessimistic: a DAC channel may still be active after a warm MCU reset
     * until the power-down below has been acknowledged. */
    s_led_on = true;
    hal_status_t st = read_back(rb);
    if (st != HAL_OK) {
        return st;
    }
    if (!eeprom_ok(rb)) {
        /* Sequential Write: 0101 0 start=A UDAC=0, then A..D. Writes the
         * input registers and the EEPROM. */
        uint8_t w[9];
        w[0] = 0x50;
        for (unsigned ch = 0; ch < 4; ch++) {
            w[1 + ch * 2] = (uint8_t)((VREF_VDD << 7) | (PD_1K << 5)); /* G=0, D=0 */
            w[2 + ch * 2] = 0;
        }
        st = drv_i2c_xfer(ADDR, w, sizeof w, NULL, 0);
        if (st != HAL_OK) {
            return st;
        }
        s_eeprom_written = true;
        uint32_t t0 = hal_millis();
        for (;;) {
            uint8_t b;
            st = drv_i2c_xfer(ADDR, NULL, 0, &b, 1);
            if (st != HAL_OK) {
                return st;
            }
            if (b & 0x80u) { /* RDY */
                break;
            }
            if ((uint32_t)(hal_millis() - t0) >= BSY_TIMEOUT_MS) {
                return HAL_ERR_TIMEOUT;
            }
            hal_delay_ms(1);
        }
        st = read_back(rb);
        if (st != HAL_OK) {
            return st;
        }
        if (!eeprom_ok(rb)) {
            return HAL_ERR_BUS;
        }
    }
    return mcp4728_power_down_all();
}

bool mcp4728_eeprom_was_written(void) { return s_eeprom_written; }

hal_status_t mcp4728_set_led_code(uint16_t code)
{
    if (code > MCP4728_MAX_CODE) {
        code = MCP4728_MAX_CODE;
    }
    uint8_t w[3];
    put_block(w, 0, VREF_INT, 0, code);
    /* Pessimistic: a transfer that reports an error may still have reached
     * the DAC (e.g. a timeout after the last data byte was acknowledged), so
     * channel A counts as on until a power-down is acknowledged. */
    s_led_on = true;
    return drv_i2c_xfer(ADDR, w, sizeof w, NULL, 0);
}

hal_status_t mcp4728_led_off(void)
{
    uint8_t w[3];
    put_block(w, 0, VREF_VDD, PD_1K, 0);
    hal_status_t st = drv_i2c_xfer(ADDR, w, sizeof w, NULL, 0);
    if (st == HAL_OK) {
        s_led_on = false;
    }
    return st;
}

bool mcp4728_led_is_on(void) { return s_led_on; }

uint16_t mcp4728_code_from_ma100(uint32_t ma100)
{
    if (ma100 > 100000u) { /* far beyond full scale; avoid overflow */
        return MCP4728_MAX_CODE;
    }
    uint32_t ua = ma100 * 10u;
    uint32_t code = (ua * 1000u + LED_UA_PER_CODE_X1000 / 2u) / LED_UA_PER_CODE_X1000;
    return (uint16_t)(code > MCP4728_MAX_CODE ? MCP4728_MAX_CODE : code);
}

uint32_t mcp4728_ma100_from_code(uint16_t code)
{
    if (code > MCP4728_MAX_CODE) {
        code = MCP4728_MAX_CODE;
    }
    /* uA = code * 3.718; mA x100 = uA / 10 */
    return ((uint32_t)code * LED_UA_PER_CODE_X1000 + 5000u) / 10000u;
}
