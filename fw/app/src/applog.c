#include "applog.h"

#include <stdbool.h>

#include "hal.h"

#define LINE_MAX 120

static char s_line[LINE_MAX + 2];
static char *s_p;
static log_tee_t s_tee;
static bool s_local; /* this line stays on RTT (not teed) */

void log_set_tee(log_tee_t tee) { s_tee = tee; }

char *fmt_s(char *b, const char *end, const char *s)
{
    while (s && *s && b < end)
        *b++ = *s++;
    *b = '\0';
    return b;
}

char *fmt_u(char *b, const char *end, uint32_t v)
{
    char t[10];
    unsigned n = 0;
    do {
        t[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v && n < sizeof t);
    while (n && b < end)
        *b++ = t[--n];
    *b = '\0';
    return b;
}

char *fmt_i(char *b, const char *end, int32_t v)
{
    if (v < 0 && b < end) {
        *b++ = '-';
        return fmt_u(b, end, (uint32_t)(-(v + 1)) + 1u);
    }
    return fmt_u(b, end, (uint32_t)v);
}

char *fmt_x(char *b, const char *end, uint32_t v, unsigned digits)
{
    static const char hx[] = "0123456789ABCDEF";
    while (digits-- && b < end)
        *b++ = hx[(v >> (4u * digits)) & 0xFu];
    *b = '\0';
    return b;
}

static const char *lend(void) { return s_line + LINE_MAX; }

void log_begin(const char *tag)
{
    s_p = s_line;
    s_local = false;
    *s_p++ = '[';
    s_p = fmt_u(s_p, lend(), hal_millis());
    s_p = fmt_s(s_p, lend(), "] ");
    s_p = fmt_s(s_p, lend(), tag);
    s_p = fmt_s(s_p, lend(), ":");
}

void log_s(const char *s) { if (s_p) s_p = fmt_s(s_p, lend(), s); }
void log_u(uint32_t v) { if (s_p) s_p = fmt_u(s_p, lend(), v); }
void log_i(int32_t v) { if (s_p) s_p = fmt_i(s_p, lend(), v); }
void log_x(uint32_t v, unsigned digits) { if (s_p) s_p = fmt_x(s_p, lend(), v, digits); }

void log_end(void)
{
    if (!s_p)
        return;
    *s_p++ = '\n';
    size_t n = (size_t)(s_p - s_line);
    s_p = 0;
    (void)hal_log_write(s_line, n);
    if (s_tee && !s_local)
        s_tee(s_line, n);
}

void log_line(const char *tag, const char *text)
{
    log_begin(tag);
    log_s(" ");
    log_s(text);
    log_end();
}

void log_line_local(const char *tag, const char *text)
{
    log_begin(tag);
    s_local = true;
    log_s(" ");
    log_s(text);
    log_end();
}
