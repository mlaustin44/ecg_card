/* Private helpers for ecg_dsp. Integer only; no library division on 64-bit
 * values (dsp_udiv64 is shift-subtract). Right shifts of negative values are
 * arithmetic (GCC / Clang / armcc behaviour; checked in the tests). */
#ifndef DSP_UTIL_H
#define DSP_UTIL_H

#include <stdint.h>

static inline int16_t dsp_sat16(int32_t v)
{
    return v > 32767 ? (int16_t)32767 : v < -32768 ? (int16_t)-32768 : (int16_t)v;
}

static inline int16_t dsp_sat16_64(int64_t v)
{
    return v > 32767 ? (int16_t)32767 : v < -32768 ? (int16_t)-32768 : (int16_t)v;
}

/* Round-to-nearest arithmetic shift right (s >= 1). */
static inline int32_t dsp_rshr32(int32_t v, unsigned s)
{
    return (v + (int32_t)(1u << (s - 1u))) >> s;
}

static inline int64_t dsp_rshr64(int64_t v, unsigned s)
{
    return (v + ((int64_t)1 << (s - 1u))) >> s;
}

static inline int32_t dsp_abs32(int32_t v) { return v < 0 ? -v : v; }

/* floor(sqrt(v)) */
uint32_t dsp_isqrt64(uint64_t v);

/* n / d for d != 0, quotient must fit 32 bits (else saturates). */
uint32_t dsp_udiv64(uint64_t n, uint32_t d);

/* Signed n / d rounded to nearest, d != 0, |quotient| < 2^31. */
int32_t dsp_sdiv64_round(int64_t n, int32_t d);

/* Median of 5 values (inputs untouched). */
uint32_t dsp_median5_u32(const uint32_t v[5]);
uint16_t dsp_median_u16(const uint16_t *v, unsigned n); /* n <= 5 */

#endif
