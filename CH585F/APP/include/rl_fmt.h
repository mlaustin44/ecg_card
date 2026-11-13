/* Small printf-style formatter for STREAM_LOG lines (portable, no WCH
 * headers, no heap, no newlib stdio). Supports %% %c %s %d %i %u %x %X
 * with an optional '0' flag, a width up to 99 and the 'l' modifier.
 * Anything else is copied literally. */
#ifndef RL_FMT_H
#define RL_FMT_H

#include <stdarg.h>
#include <stddef.h>

/* Writes at most size-1 characters plus a terminator (if size > 0) and
 * returns the number of characters written (not counting the terminator). */
size_t rl_vfmt(char *out, size_t size, const char *fmt, va_list ap);
size_t rl_fmt(char *out, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif
