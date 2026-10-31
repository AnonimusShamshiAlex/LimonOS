#include "libc.h"

int snprintf(char *out, size_t cap, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

static void write_all(const char *p, size_t n) {
    while (n) {
        long w = write(1, p, n);
        if (w <= 0) return;
        p += w; n -= (size_t)w;
    }
}

int printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t w = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1;    /* длиннее буфера — обрезается */
    write_all(buf, w);
    return n;
}
int puts(const char *s) { write_all(s, strlen(s)); write_all("\n", 1); return 1; }
int putchar(int c) { char ch = (char)c; write_all(&ch, 1); return c; }
