/* Дифференциальный тест: vsnprintf из userland/fmt.c против glibc на большом наборе форматов.
 * Сравниваются И текст, И возвращаемое значение, И усечение при малом буфере. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#define FMT_STANDALONE_NAME limon_vsnprintf
#include "../userland/fmt.c"

static int fails, total;
static int run_both(const char *fmt, size_t cap, ...) {
    char a[256], b[256];
    memset(a, 'A', sizeof a); memset(b, 'A', sizeof b);
    va_list v1, v2; va_start(v1, cap); va_copy(v2, v1);
    int ra = limon_vsnprintf(a, cap, fmt, v1);
    int rb = vsnprintf(b, cap, fmt, v2);
    va_end(v1); va_end(v2);
    total++;
    if (ra != rb || memcmp(a, b, 256) != 0) {
        if (fails++ < 15) printf("РАЗНИЦА fmt=\"%s\" cap=%zu: наш(%d)=\"%.40s\" glibc(%d)=\"%.40s\"\n", fmt, cap, ra, a, rb, b);
        return 1;
    }
    return 0;
}

int main(void) {
    const long long ivals[] = { 0, 1, -1, 7, -7, 42, 255, 256, 65535, 123456789, -123456789, INT_MAX, INT_MIN, LLONG_MAX, LLONG_MIN };
    const char *flags[] = { "", "-", "0", "+", " ", "-0", "+0", "# ", "#", "-+" };
    const char *widths[] = { "", "1", "5", "12", "*" };
    const char *precs[] = { "", ".0", ".1", ".5", ".12", ".*" };
    const char *lens[] = { "", "h", "hh", "l", "ll", "z" };
    const char *convs = "diuxXo";
    char fmt[64];
    for (const char *cv = convs; *cv; cv++)
    for (size_t fi = 0; fi < sizeof flags / sizeof *flags; fi++)
    for (size_t wi = 0; wi < sizeof widths / sizeof *widths; wi++)
    for (size_t pi = 0; pi < sizeof precs / sizeof *precs; pi++)
    for (size_t li = 0; li < sizeof lens / sizeof *lens; li++) {
        snprintf(fmt, sizeof fmt, "[%%%s%s%s%s%c]", flags[fi], widths[wi], precs[pi], lens[li], *cv);
        int w = 7, p = 3;
        for (size_t k = 0; k < sizeof ivals / sizeof *ivals; k++) {
            long long v = ivals[k];
            int hasw = widths[wi][0] == '*', hasp = precs[pi][0] == '.' && precs[pi][1] == '*';
            int is_l = lens[li][0] == 'l' && lens[li][1] == 'l';
            int is_l1 = lens[li][0] == 'l' && !lens[li][1];
            int is_z = lens[li][0] == 'z';
            #define CALL(T) do { T x = (T)v; \
                if (hasw && hasp) run_both(fmt, 256, w, p, x); else if (hasw) run_both(fmt, 256, w, x); \
                else if (hasp) run_both(fmt, 256, p, x); else run_both(fmt, 256, x); } while (0)
            if (is_l) CALL(long long); else if (is_l1 || is_z) CALL(long); else CALL(int);
        }
    }
    /* строки, символы, указатели, %% и усечение */
    const char *strs[] = { "", "a", "limon", "Привет", "0123456789abcdef0123456789" };
    const char *sf[] = { "%s", "%10s", "%-10s", "%.3s", "%10.3s", "%-10.3s", "%.0s", "%5.10s" };
    for (size_t i = 0; i < sizeof sf / sizeof *sf; i++) for (size_t j = 0; j < sizeof strs / sizeof *strs; j++) run_both(sf[i], 256, strs[j]);
    run_both("%s", 256, (const char *)NULL);                   /* "(null)" у обоих */
    const char *cf[] = { "%c", "%3c", "%-3c" };
    for (size_t i = 0; i < 3; i++) { run_both(cf[i], 256, 'x'); run_both(cf[i], 256, 'Z'); }
    run_both("100%%", 256); run_both("%5%|", 256);
    run_both("%p", 256, (void *)0); run_both("%p", 256, (void *)0x1234abcd); run_both("%20p|", 256, (void *)0xdeadbeef);
    /* усечение: все длины буфера от 0 до 20 для смешанной строки */
    for (size_t cap = 0; cap <= 20; cap++) {
        run_both("abc %d def %s ghi %x", cap, -12345, "string", 0xbeef);
        run_both("%s", cap, "0123456789012345678901234567890");
        run_both("", cap);
    }
    printf("fmt diff: %d сравнений, расхождений %d\n", total, fails);
    return fails != 0;
}
