/* Internal: I2C transfer with one retry after bus recovery. */
#ifndef DRV_I2C_H
#define DRV_I2C_H

#include "hal.h"

/* HAL_ERR_BUS / HAL_ERR_TIMEOUT: recover the bus and retry once.
 * HAL_ERR_NACK is returned as is (no retry). */
static inline hal_status_t drv_i2c_xfer(uint8_t addr, const uint8_t *w, size_t wn,
                                        uint8_t *r, size_t rn)
{
    hal_status_t st = hal_i2c_xfer(addr, w, wn, r, rn);
    if (st == HAL_ERR_BUS || st == HAL_ERR_TIMEOUT) {
        hal_i2c_recover();
        st = hal_i2c_xfer(addr, w, wn, r, rn);
    }
    return st;
}

#endif
