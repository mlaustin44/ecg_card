/* Tiny printf-free text log on RTT channel 0 (hal_log_write). One line is
 * assembled in a static buffer and written at once, prefixed with the
 * uptime in ms; it is dropped if the host is not reading (never blocks).
 *
 *   log_begin("acq"); log_s(" vmid="); log_u(mv); log_end(); */
#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdint.h>

void log_begin(const char *tag);
void log_s(const char *s);
void log_u(uint32_t v);
void log_i(int32_t v);
void log_x(uint32_t v, unsigned digits);
void log_end(void);
/* One-liner: "[t] tag: text" */
void log_line(const char *tag, const char *text);

/* Formatting helpers shared with the diagnostics pages: write into b,
 * return the end (NUL-terminated). */
char *fmt_s(char *b, const char *end, const char *s);
char *fmt_u(char *b, const char *end, uint32_t v);
char *fmt_i(char *b, const char *end, int32_t v);
char *fmt_x(char *b, const char *end, uint32_t v, unsigned digits);

#endif
