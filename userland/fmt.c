/* vsnprintf — отдельный файл без зависимостей от остальной libc: так его можно сравнить с glibc
 * на хосте (tests/fmt_diff_host.c). Поддержано: %d %i %u %x %X %o %c %s %p %%, флаги '-' '0' '+' ' ',
 * ширина и точность (в том числе *), длины hh h l ll z. Не поддержано: плавающая точка (%f/%g/%e). */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef FMT_STANDALONE_NAME
#define VSNPRINTF FMT_STANDALONE_NAME
#else
#define VSNPRINTF vsnprintf
#endif

struct out { char *p; size_t cap, n; };
static void put(struct out *o, char c) { if (o->n + 1 < o->cap) o->p[o->n] = c; o->n++; }

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int VSNPRINTF(char *buf, size_t cap, const char *fmt, va_list ap) {
    struct out o = { buf, cap, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { put(&o, *fmt); continue; }
        fmt++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1; else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1; else if (*fmt == ' ') space = 1; else if (*fmt == '#') alt = 1;
            else break;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        int lng = 0;                            /* 0=int, 1=long, 2=long long, -1=short, -2=char */
        for (;; fmt++) {
            if (*fmt == 'l') lng = lng >= 1 ? 2 : 1;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') lng = 1;
            else if (*fmt == 'h') lng = lng <= -1 ? -2 : -1;
            else break;
        }
        char c = *fmt;
        if (!c) break;
        if (c == '%') { put(&o, '%'); continue; }

        char tmp[72]; const char *s = tmp; size_t len = 0; char sign = 0; const char *prefix = ""; int numeric = 0;
        if (c == 'c') { tmp[0] = (char)va_arg(ap, int); len = 1; }
        else if (c == 's') {
            s = va_arg(ap, const char *); if (!s) s = "(null)";
            len = slen(s); if (prec >= 0 && (size_t)prec < len) len = (size_t)prec;
        } else if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X' || c == 'o' || c == 'p') {
            unsigned long long v; int base = 10; int upper = (c == 'X'); int neg = 0;
            if (c == 'p') { v = (uintptr_t)va_arg(ap, void *); base = 16; prefix = "0x"; if (!v) { prefix = ""; s = "(nil)"; len = 5; goto emit_str; } }
            else if (c == 'd' || c == 'i') {
                long long sv = lng >= 2 ? va_arg(ap, long long) : lng == 1 ? va_arg(ap, long) : va_arg(ap, int);
                if (lng == -1) sv = (short)sv; else if (lng == -2) sv = (signed char)sv;
                if (sv < 0) { neg = 1; v = 0ULL - (unsigned long long)sv; } else v = (unsigned long long)sv;
                if (neg) sign = '-'; else if (plus) sign = '+'; else if (space) sign = ' ';
            } else {
                v = lng >= 2 ? va_arg(ap, unsigned long long) : lng == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                if (lng == -1) v = (unsigned short)v; else if (lng == -2) v = (unsigned char)v;
                base = (c == 'u') ? 10 : (c == 'o') ? 8 : 16;
                if (alt && v && c == 'x') prefix = "0x"; else if (alt && v && c == 'X') prefix = "0X";
            }
            numeric = 1;
            char rev[72]; size_t rl = 0;
            const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
            if (v == 0) { if (prec != 0) rev[rl++] = '0'; }
            while (v) { rev[rl++] = dig[v % (unsigned)base]; v /= (unsigned)base; }
            while ((int)rl < prec && rl < sizeof rev - 1) rev[rl++] = '0';       /* точность = минимум цифр */
            if (alt && c == 'o' && (rl == 0 || rev[rl - 1] != '0')) rev[rl++] = '0';   /* %#o: ведущий ноль, если его ещё нет */
            for (size_t i = 0; i < rl; i++) tmp[i] = rev[rl - 1 - i];
            len = rl;
        } else { put(&o, '%'); put(&o, c); continue; }       /* неизвестное преобразование — как есть */
emit_str:;
        size_t pre = slen(prefix) + (sign ? 1 : 0);
        size_t total = pre + len;
        size_t pad = (width > 0 && (size_t)width > total) ? (size_t)width - total : 0;
        int zpad = zero && !left && !(numeric && prec >= 0) && c != 's' && c != 'c';   /* 0 игнорируется при точности и для строк */
        if (!left && !zpad) while (pad--) put(&o, ' ');
        if (sign) put(&o, sign);
        for (const char *q = prefix; *q; q++) put(&o, *q);
        if (zpad) while (pad--) put(&o, '0');
        for (size_t i = 0; i < len; i++) put(&o, s[i]);
        if (left) while (pad--) put(&o, ' ');
    }
    if (cap) o.p[o.n < cap ? o.n : cap - 1] = 0;
    return (int)o.n;
}
