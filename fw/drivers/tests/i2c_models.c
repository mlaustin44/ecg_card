#include "i2c_models.h"

#include <string.h>

/* ---- MCP4728 ----------------------------------------------------------- */

static void dec_reg(dac_reg_t *d, uint8_t b1, uint8_t b2)
{
    d->vref = b1 >> 7;
    d->pd = (b1 >> 5) & 3;
    d->gx = (b1 >> 4) & 1;
    d->val = (uint16_t)(((b1 & 0x0F) << 8) | b2);
}

static void enc_reg(const dac_reg_t *d, uint8_t *o)
{
    o[0] = (uint8_t)((d->vref << 7) | (d->pd << 5) | (d->gx << 4) | (d->val >> 8));
    o[1] = (uint8_t)d->val;
}

static hal_status_t mcp_xfer(void *ctx, const uint8_t *w, size_t wn, uint8_t *r,
                             size_t rn)
{
    mcp4728_model_t *m = ctx;
    bool busy = (int32_t)(m->busy_until - mock_now) > 0 || m->never_busy;
    if (wn) {
        m->writes++;
        m->last_wn = wn < sizeof m->last_w ? wn : sizeof m->last_w;
        memcpy(m->last_w, w, m->last_wn);
        if (busy) {
            m->write_while_busy++;
        }
        size_t i = 0;
        uint8_t c = w[0];
        if ((c & 0xF8) == 0x40) { /* Multi-Write, repeated blocks */
            while (i + 3 <= wn && (w[i] & 0xF8) == 0x40) {
                dec_reg(&m->in[(w[i] >> 1) & 3], w[i + 1], w[i + 2]);
                i += 3;
            }
            if (i != wn) {
                m->unknown_cmds++;
            }
        } else if ((c & 0xF8) == 0x50) { /* Sequential Write (EEPROM) */
            unsigned ch = (c >> 1) & 3;
            m->seq_wn = wn < sizeof m->seq_w ? wn : sizeof m->seq_w;
            memcpy(m->seq_w, w, m->seq_wn);
            i = 1;
            while (i + 2 <= wn && ch < 4) {
                dec_reg(&m->in[ch], w[i], w[i + 1]);
                if (!m->corrupt_eeprom) {
                    m->ee[ch] = m->in[ch];
                }
                ch++;
                i += 2;
            }
            m->eeprom_writes++;
            m->busy_until = mock_now + 50;
        } else if ((c & 0xF8) == 0x58) { /* Single Write (EEPROM) */
            unsigned ch = (c >> 1) & 3;
            dec_reg(&m->in[ch], w[1], w[2]);
            m->ee[ch] = m->in[ch];
            m->eeprom_writes++;
            m->busy_until = mock_now + 50;
        } else {
            m->unknown_cmds++;
        }
    }
    if (rn) {
        m->reads++;
        uint8_t full[24];
        for (unsigned ch = 0; ch < 4; ch++) {
            for (unsigned k = 0; k < 2; k++) {
                uint8_t *o = &full[ch * 6 + k * 3];
                o[0] = (uint8_t)((busy ? 0 : 0x80) | (ch << 4));
                enc_reg(k ? &m->ee[ch] : &m->in[ch], &o[1]);
            }
        }
        for (size_t i = 0; i < rn && i < sizeof full; i++) {
            r[i] = full[i];
        }
    }
    return HAL_OK;
}

void mcp4728_model_attach(mcp4728_model_t *m)
{
    memset(m, 0, sizeof *m);
    for (int i = 0; i < 4; i++) { /* blank part: VDD ref, PD 0, mid-scale */
        m->in[i] = m->ee[i] = (dac_reg_t){0, 0, 0, 0x800};
    }
    mock_i2c_attach(I2C_ADDR_MCP4728, mcp_xfer, m);
}

void mcp4728_model_set_good_eeprom(mcp4728_model_t *m)
{
    for (int i = 0; i < 4; i++) {
        m->ee[i] = (dac_reg_t){1, 1, 0, 0};
    }
}

/* ---- CW2015 ------------------------------------------------------------ */

static hal_status_t cw_xfer(void *ctx, const uint8_t *w, size_t wn, uint8_t *r,
                            size_t rn)
{
    cw2015_model_t *m = ctx;
    if (wn) {
        m->ptr = w[0];
        for (size_t i = 1; i < wn; i++) {
            uint8_t reg = (uint8_t)(m->ptr);
            if (reg >= 0x10 && reg <= 0x4F) {
                m->bad_writes++;
            }
            if (m->wlog_n < 64) {
                m->wlog[m->wlog_n].reg = reg;
                m->wlog[m->wlog_n].val = w[i];
                m->wlog_n++;
            }
            if (reg < sizeof m->reg && reg != 0x00) {
                m->reg[reg] = w[i];
            }
            m->ptr++;
        }
    }
    for (size_t i = 0; i < rn; i++) {
        uint8_t reg = m->ptr++;
        r[i] = reg < sizeof m->reg ? m->reg[reg] : 0;
        if (reg == 0 && m->fail_version) {
            r[i] = 0x00;
        }
    }
    return HAL_OK;
}

void cw2015_model_attach(cw2015_model_t *m)
{
    memset(m, 0, sizeof *m);
    m->reg[0x00] = 0x73;
    m->reg[0x08] = 0x02 | 0x80; /* UFG set by someone else, ATHD = 16 */
    m->reg[0x0A] = 0xC0;
    mock_i2c_attach(I2C_ADDR_CW2015, cw_xfer, m);
}

void cw2015_model_set_vcell(cw2015_model_t *m, uint16_t raw14, uint8_t soc)
{
    m->reg[0x02] = (uint8_t)(raw14 >> 8);
    m->reg[0x03] = (uint8_t)raw14;
    m->reg[0x04] = soc;
}
