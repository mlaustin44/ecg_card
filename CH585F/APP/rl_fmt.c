/* Small printf-style formatter for STREAM_LOG lines. Portable C. */
#include "rl_fmt.h"

typedef struct {
    char *out;
    size_t size, n;
} sink_t;

static void put(sink_t *s, char c)
{
    if (s->n + 1u < s->size)
        s->out[s->n++] = c;
}

static void put_num(sink_t *s, unsigned long v, unsigned base, int upper, int neg,
                    unsigned width, char pad)
{
    char tmp[24];
    unsigned i = 0, len;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    do {
        tmp[i++] = dig[v % base];
        v /= base;
    } while (v);
    len = i + (neg ? 1u : 0u);
    if (neg && pad == '0')
        put(s, '-');
    while (width > len) {
        put(s, pad);
        width--;
    }
    if (neg && pad != '0')
        put(s, '-');
    while (i)
        put(s, tmp[--i]);
}

size_t rl_vfmt(char *out, size_t size, const char *fmt, va_list ap)
{
    sink_t s;
    s.out = out;
    s.size = size;
    s.n = 0;

    while (*fmt) {
        char c = *fmt++, pad = ' ';
        unsigned width = 0;
        int is_long = 0;

        if (c != '%') {
            put(&s, c);
            continue;
        }
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            if (width < 100u)
                width = width * 10u + (unsigned)(*fmt - '0');
            fmt++;
        }
        if (width > 99u)
            width = 99u;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }
        c = *fmt;
        if (c == '\0')
            break;
        fmt++;
        switch (c) {
        case '%':
            put(&s, '%');
            break;
        case 'c':
            put(&s, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str)
                str = "(null)";
            while (*str)
                put(&s, *str++);
            break;
        }
        case 'd':
        case 'i': {
            long v = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
            unsigned long m = v < 0 ? 0ul - (unsigned long)v : (unsigned long)v;
            put_num(&s, m, 10u, 0, v < 0, width, pad);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            unsigned long v = is_long ? va_arg(ap, unsigned long)
                                      : (unsigned long)va_arg(ap, unsigned);
            put_num(&s, v, c == 'u' ? 10u : 16u, c == 'X', 0, width, pad);
            break;
        }
        default: /* unsupported: copy literally */
            put(&s, '%');
            put(&s, c);
            break;
        }
    }
    if (size)
        out[s.n] = '\0';
    return s.n;
}

size_t rl_fmt(char *out, size_t size, const char *fmt, ...)
{
    va_list ap;
    size_t n;
    va_start(ap, fmt);
    n = rl_vfmt(out, size, fmt, ap);
    va_end(ap);
    return n;
}
