#include "format.h"
#include "kstring.h"

static void pad_out(fmt_put put, void *ctx, char c, int n) {
    while (n-- > 0) put(c, ctx);
}

static void num_out(fmt_put put, void *ctx, unsigned long long v, int neg, int base,
                    int upper, int width, char pad, int left) {
    char buf[24];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) buf[n++] = '0';
    while (v > 0) { buf[n++] = digits[v % (unsigned)base]; v /= (unsigned)base; }
    int total = n + (neg ? 1 : 0);
    int padn = width > total ? width - total : 0;
    if (left) {
        if (neg) put('-', ctx);
        while (n) put(buf[--n], ctx);
        pad_out(put, ctx, ' ', padn);
    } else if (pad == '0') {
        if (neg) put('-', ctx);
        pad_out(put, ctx, '0', padn);
        while (n) put(buf[--n], ctx);
    } else {
        pad_out(put, ctx, ' ', padn);
        if (neg) put('-', ctx);
        while (n) put(buf[--n], ctx);
    }
}

void kvformat(fmt_put put, void *ctx, const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(*fmt, ctx); continue; }
        fmt++;
        char pad = ' ';
        int left = 0, width = 0, longs = 0, prec = -1;
        for (;; fmt++) {
            if (*fmt == '0') pad = '0';
            else if (*fmt == '-') left = 1;
            else break;
        }
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {                                   /* точность: %.47s, %.*s */
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); if (prec < 0) prec = -1; fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        for (;;) {                                           /* l, ll, z, h — размер аргумента */
            if (*fmt == 'l') { longs++; fmt++; }
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { longs = 1; fmt++; }
            else if (*fmt == 'h') fmt++;
            else break;
        }
        switch (*fmt) {
        case 'c': put((char)va_arg(ap, int), ctx); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            size_t nbytes = 0; int len = 0;                  /* считаем байты и символы UTF-8 в пределах точности */
            while (s[nbytes] && (prec < 0 || (int)nbytes < prec)) {
                unsigned char c = (unsigned char)s[nbytes];
                int cl = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
                if (prec >= 0 && (int)nbytes + cl > prec) break;   /* не режем символ пополам */
                int real = 1;                                  /* оборванный символ в конце строки не уводит за NUL */
                while (real < cl && s[nbytes + (size_t)real]) real++;
                nbytes += (size_t)real; len++;
            }
            if (!left) pad_out(put, ctx, ' ', width - len);
            for (size_t i = 0; i < nbytes && s[i]; i++) put(s[i], ctx);
            if (left) pad_out(put, ctx, ' ', width - len);
            break;
        }
        case 'd': case 'i': {
            long long v = longs >= 2 ? va_arg(ap, long long)
                        : longs == 1 ? va_arg(ap, long) : va_arg(ap, int);
            int neg = v < 0;
            unsigned long long u = neg ? (unsigned long long)(-(v + 1)) + 1 : (unsigned long long)v;
            num_out(put, ctx, u, neg, 10, 0, width, pad, left);
            break;
        }
        case 'u': case 'x': case 'X': {
            unsigned long long v = longs >= 2 ? va_arg(ap, unsigned long long)
                                 : longs == 1 ? va_arg(ap, unsigned long)
                                 : va_arg(ap, unsigned int);
            num_out(put, ctx, v, 0, *fmt == 'u' ? 10 : 16, *fmt == 'X', width, pad, left);
            break;
        }
        case 'p':
            put('0', ctx); put('x', ctx);
            num_out(put, ctx, (unsigned long long)(uintptr_t)va_arg(ap, void *), 0, 16, 0, 16, '0', 0);
            break;
        case '%': put('%', ctx); break;
        case '\0': return;
        default: put('%', ctx); put(*fmt, ctx); break;
        }
    }
}

struct sn_ctx { char *buf; size_t size, pos; };

static void sn_put(char c, void *vctx) {
    struct sn_ctx *s = vctx;
    if (s->pos + 1 < s->size) s->buf[s->pos] = c;
    s->pos++;
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct sn_ctx s = { buf, size, 0 };
    kvformat(sn_put, &s, fmt, ap);
    if (size) buf[s.pos < size ? s.pos : size - 1] = 0;
    return (int)s.pos;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
