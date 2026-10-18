#ifndef CONSOLE_H
#define CONSOLE_H
#include <stdint.h>

void console_init(uint32_t fg, uint32_t bg);
void console_clear(void);
void console_set_color(uint32_t fg, uint32_t bg);
void console_putc(char c);           /* принимает байты UTF-8 по одному */
void console_write(const char *s);

/* Мини-printf: %c %s %d %i %u %x %X %p %%, модификаторы l/ll, флаг 0, ширина */
void kprintf(const char *fmt, ...);

#endif
