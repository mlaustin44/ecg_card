/* Register-level I2C device models for the tests. */
#ifndef I2C_MODELS_H
#define I2C_MODELS_H

#include "mock_hal.h"

typedef struct {
    bool vref;
    uint8_t pd, gx;
    uint16_t val;
} dac_reg_t;

typedef struct {
    dac_reg_t in[4], ee[4];
    uint32_t busy_until;     /* mock time */
    int eeprom_writes;       /* number of EEPROM program commands */
    int unknown_cmds;
    int write_while_busy;
    int never_busy;          /* if set, BSY stays low forever (timeout test) */
    int corrupt_eeprom;      /* if set, EEPROM writes are dropped (verify test) */
    int reads;
    uint8_t last_w[64];
    size_t last_wn;
    int writes;
    uint8_t seq_w[16];
    size_t seq_wn;
} mcp4728_model_t;

void mcp4728_model_attach(mcp4728_model_t *m); /* sets power-on defaults of a blank part */
void mcp4728_model_set_good_eeprom(mcp4728_model_t *m);

typedef struct {
    uint8_t reg[0x60];
    uint8_t ptr;
    int bad_writes;          /* writes to 0x10..0x4F */
    struct { uint8_t reg, val; } wlog[64];
    int wlog_n;
    int fail_version;
} cw2015_model_t;

void cw2015_model_attach(cw2015_model_t *m);
void cw2015_model_set_vcell(cw2015_model_t *m, uint16_t raw14, uint8_t soc);

#endif
