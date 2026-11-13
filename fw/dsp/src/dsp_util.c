#include "dsp_util.h"

uint32_t dsp_isqrt64(uint64_t v)
{
    uint64_t r = 0;
    uint64_t bit = (uint64_t)1 << 62;
    while (bit > v)
        bit >>= 2;
    while (bit != 0) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

uint32_t dsp_udiv64(uint64_t n, uint32_t d)
{
    if (d == 0)
        return UINT32_MAX;
    if ((n >> 32) >= d)
        return UINT32_MAX; /* quotient would not fit */
    uint64_t rem = 0;
    uint32_t q = 0;
    for (int i = 63; i >= 0; i--) {
        rem = (rem << 1) | ((n >> i) & 1u);
        q <<= 1;
        if (rem >= d) {
            rem -= d;
            q |= 1u;
        }
    }
    return q;
}

int32_t dsp_sdiv64_round(int64_t n, int32_t d)
{
    int neg = 0;
    uint64_t un;
    uint32_t ud;
    if (n < 0) {
        un = (uint64_t)(-n);
        neg = !neg;
    } else {
        un = (uint64_t)n;
    }
    if (d < 0) {
        ud = (uint32_t)(-(int64_t)d);
        neg = !neg;
    } else {
        ud = (uint32_t)d;
    }
    uint32_t q = dsp_udiv64(un + (ud >> 1), ud);
    if (q > (uint32_t)INT32_MAX)
        q = (uint32_t)INT32_MAX;
    return neg ? -(int32_t)q : (int32_t)q;
}

static void sort_u32(uint32_t *a, unsigned n)
{
    for (unsigned i = 1; i < n; i++) {
        uint32_t v = a[i];
        unsigned j = i;
        while (j > 0 && a[j - 1] > v) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = v;
    }
}

uint32_t dsp_median5_u32(const uint32_t v[5])
{
    uint32_t a[5] = { v[0], v[1], v[2], v[3], v[4] };
    sort_u32(a, 5);
    return a[2];
}

uint16_t dsp_median_u16(const uint16_t *v, unsigned n)
{
    uint32_t a[5];
    if (n == 0)
        return 0;
    if (n > 5)
        n = 5;
    for (unsigned i = 0; i < n; i++)
        a[i] = v[i];
    sort_u32(a, n);
    /* even n: lower-middle average */
    return (n & 1u) ? (uint16_t)a[n / 2] : (uint16_t)((a[n / 2 - 1] + a[n / 2] + 1u) / 2u);
}
