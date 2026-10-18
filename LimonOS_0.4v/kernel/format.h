#ifndef LIMON_FORMAT_H
#define LIMON_FORMAT_H
#include <stdarg.h>
#include <stddef.h>

typedef void (*fmt_put)(char c, void *ctx);

/* Мини-printf: %c %s %d %i %u %x %X %p %%, l/ll, флаги 0 и -, ширина.
 * Ширина строк считается в символах UTF-8 (русский выравнивается правильно). */
void kvformat(fmt_put put, void *ctx, const char *fmt, va_list ap);
int  ksnprintf(char *buf, size_t size, const char *fmt, ...);
int  kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#endif
