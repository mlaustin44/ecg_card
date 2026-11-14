/* Tiny printf-free text log on RTT channel 0 (hal_log_write). One line is
 * assembled in a static buffer and written at once, prefixed with the
 * uptime in ms; it is dropped if the host is not reading (never blocks).
 * A tee (log_set_tee) may copy each finished line elsewhere: the radio link
 * forwards it to a phone subscribed to the Log characteristic.
 *
 *   log_begin("acq"); log_s(" vmid="); log_u(mv); log_end(); */
#ifndef APP_LOG_H
#define APP_LOG_H

#include <stddef.h>
#include <stdint.h>

void log_begin(const char *tag);
void log_s(const char *s);
void log_u(uint32_t v);
void log_i(int32_t v);
void log_x(uint32_t v, unsigned digits);
void log_end(void);
/* One-liner: "[t] tag: text" */
void log_line(const char *tag, const char *text);
/* The same, but never handed to the tee (text that came from the tee's
 * destination: the radio's own log lines). */
void log_line_local(const char *tag, const char *text);

/* Tee: called with every finished line (incl. its '\n', not NUL-terminated)
 * right after it went to RTT, which it cannot change. It must not log and
 * must not keep the pointer. NULL (the default) removes it. */
typedef void (*log_tee_t)(const char *line, size_t n);
void log_set_tee(log_tee_t tee);

/* Formatting helpers shared with the diagnostics pages: write into b,
 * return the end (NUL-terminated). */
char *fmt_s(char *b, const char *end, const char *s);
char *fmt_u(char *b, const char *end, uint32_t v);
char *fmt_i(char *b, const char *end, int32_t v);
char *fmt_x(char *b, const char *end, uint32_t v, unsigned digits);

#endif
