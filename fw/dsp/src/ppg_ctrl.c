#include "dsp_ppg_ctrl.h"

#include "board.h"

uint16_t dsp_ppg_dcc_code(uint16_t tia_on)
{
    uint32_t c = (2048u + 10u * tia_on + 5u) / 11u; /* about once per second */
    return (uint16_t)(c > 4095u ? 4095u : c);
}

bool dsp_ppg_dcc_update(uint16_t tia_on, uint16_t hi_on, uint16_t *dac12)
{
    if (hi_on + DSP_DCC_DEADBAND >= 2048u && hi_on <= 2048u + DSP_DCC_DEADBAND)
        return false;
    uint16_t c = dsp_ppg_dcc_code(tia_on);
    if (c == *dac12)
        return false;
    *dac12 = c;
    return true;
}

uint16_t dsp_ppg_dcc_predict(uint16_t tia_dark, uint16_t tia_on, uint16_t led_old, uint16_t led_new)
{
    if (led_old == 0 || tia_on <= tia_dark)
        return dsp_ppg_dcc_code(tia_on);
    uint32_t sig = (uint32_t)(tia_on - tia_dark) * led_new / led_old; /* per AGC step */
    uint32_t t = tia_dark + sig;
    return dsp_ppg_dcc_code((uint16_t)(t > 4095u ? 4095u : t));
}

uint16_t dsp_led_max_code(uint8_t limit_ma)
{
    uint32_t c = (uint32_t)limit_ma * 1000000u / (uint32_t)LED_UA_PER_CODE_X1000;
    return (uint16_t)(c > 4095u ? 4095u : c);
}

void dsp_agc_init(dsp_agc_t *a)
{
    a->dir = 0;
}

bool dsp_agc_step(dsp_agc_t *a, uint16_t tia_on, uint16_t *led_code, uint16_t max_code)
{
    if (a->dir == 0) {
        if (tia_on > DSP_AGC_HIGH)
            a->dir = -1;
        else if (tia_on < DSP_AGC_LOW)
            a->dir = 1;
    }
    if (a->dir > 0 && tia_on >= DSP_AGC_TGT_LO)
        a->dir = 0;
    if (a->dir < 0 && tia_on <= DSP_AGC_TGT_HI)
        a->dir = 0;
    if (max_code < DSP_AGC_MIN_CODE)
        max_code = DSP_AGC_MIN_CODE;
    uint32_t c = *led_code;
    if (a->dir > 0) {
        uint32_t up = c / 4u;
        c += up ? up : 1u;
    } else if (a->dir < 0) {
        uint32_t dn = c / 5u;
        c -= dn ? dn : 1u;
    }
    if (c > max_code)
        c = max_code;
    if (c < DSP_AGC_MIN_CODE)
        c = DSP_AGC_MIN_CODE;
    if (c == *led_code)
        return false;
    *led_code = (uint16_t)c;
    return true;
}

void dsp_finger_init(dsp_finger_t *f)
{
    f->present = 0;
    f->count = 0;
}

bool dsp_finger_update(dsp_finger_t *f, int16_t coarse, uint16_t tia_on)
{
    bool want;
    if (f->present)
        want = !(coarse < DSP_FINGER_OFF || tia_on >= DSP_FINGER_RAIL);
    else
        want = coarse > DSP_FINGER_ON && tia_on < DSP_FINGER_RAIL;
    if (want == (f->present != 0)) {
        f->count = 0;
    } else if (++f->count >= DSP_FINGER_HOLD) {
        f->present = want ? 1u : 0u;
        f->count = 0;
    }
    return f->present != 0;
}
