#include "libc.h"
/* Самопроверка libc. Собирается той же libc (без glibc) и на хосте запускается как настоящий процесс. */
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL строка %d: %s\n", __LINE__, #c); fails++; } } while (0)

static unsigned rng = 12345;
static unsigned rnd(void) { rng = rng * 1103515245u + 12345u; return (rng >> 8) & 0xFFFFFF; }

#define N 64
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    /* строки */
    CHECK(strlen("") == 0 && strlen("abc") == 3);
    CHECK(strcmp("a", "a") == 0 && strcmp("a", "b") < 0 && strcmp("b", "a") > 0 && strcmp("ab", "a") > 0);
    CHECK(strcmp("\xff", "a") > 0);                         /* сравнение как unsigned char */
    CHECK(strncmp("abcd", "abxx", 2) == 0 && strncmp("abcd", "abxx", 3) < 0 && strncmp("a", "b", 0) == 0);
    char buf[32]; strcpy(buf, "hello"); CHECK(strcmp(buf, "hello") == 0);
    { const char *s = "hello"; CHECK(strchr(s, 'l') == s + 2 && strchr(s, 'z') == 0 && strchr(s, 0) == s + 5); }
    memset(buf, 'x', 8); CHECK(buf[0] == 'x' && buf[7] == 'x');
    CHECK(memcmp("abc", "abd", 3) < 0 && memcmp("abc", "abc", 3) == 0 && memcmp("a", "b", 0) == 0);
    /* memmove с перекрытием в обе стороны */
    char m[16]; for (int i = 0; i < 16; i++) m[i] = (char)i;
    memmove(m + 2, m, 8); { int ok = 1; for (int i = 0; i < 8; i++) if (m[2 + i] != i) ok = 0; CHECK(ok); }
    for (int i = 0; i < 16; i++) m[i] = (char)i;
    memmove(m, m + 3, 8); { int ok = 1; for (int i = 0; i < 8; i++) if (m[i] != i + 3) ok = 0; CHECK(ok); }
    CHECK(atoi("123") == 123 && atoi("-45") == -45 && atoi("  +7x") == 7 && atoi("") == 0 && abs(-3) == 3);

    /* snprintf: усечение и возвращаемое значение */
    char o[8]; int r = snprintf(o, sizeof o, "%d-%s", 12345, "abcdef");
    CHECK(r == 12 && strcmp(o, "12345-a") == 0);
    CHECK(snprintf(0, 0, "%s", "abc") == 3);

    /* malloc: случайные malloc/free/realloc; каждый блок заполнен своим шаблоном, проверяется целостность */
    void *p[N]; size_t sz[N]; unsigned char pat[N];
    for (int i = 0; i < N; i++) p[i] = 0, sz[i] = 0;
    for (int it = 0; it < 4000; it++) {
        int i = (int)(rnd() % N);
        int op = (int)(rnd() % 3);
        if (p[i]) {                                           /* проверка содержимого ПЕРЕД любой операцией */
            unsigned char *q = p[i]; int ok = 1;
            for (size_t k = 0; k < sz[i]; k++) if (q[k] != pat[i]) { ok = 0; break; }
            if (!ok) { printf("FAIL: блок %d испорчен на итерации %d\n", i, it); fails++; break; }
        }
        if (op == 0 || !p[i]) {
            free(p[i]); size_t n = 1 + rnd() % 300;
            p[i] = malloc(n);
            if (!p[i]) { sz[i] = 0; continue; }               /* куча кончилась — допустимо */
            CHECK(((unsigned long)p[i] & 15) == 0);
            sz[i] = n; pat[i] = (unsigned char)(rnd() | 1); memset(p[i], pat[i], n);
        } else if (op == 1) { free(p[i]); p[i] = 0; sz[i] = 0; }
        else {
            size_t n = 1 + rnd() % 500;
            void *q = realloc(p[i], n);
            if (!q) continue;                                  /* при неудаче старый блок остаётся */
            size_t keep = n < sz[i] ? n : sz[i];
            unsigned char *c = q; int ok = 1;
            for (size_t k = 0; k < keep; k++) if (c[k] != pat[i]) { ok = 0; break; }
            CHECK(ok);
            p[i] = q; sz[i] = n; memset(p[i], pat[i], n);
        }
    }
    /* никакие два живых блока не пересекаются */
    for (int a = 0; a < N; a++) for (int b = a + 1; b < N; b++) {
        if (!p[a] || !p[b]) continue;
        unsigned long A = (unsigned long)p[a], B = (unsigned long)p[b];
        CHECK(A + sz[a] <= B || B + sz[b] <= A);
    }
    for (int i = 0; i < N; i++) free(p[i]);
    /* после освобождения всего память переиспользуется, а не растёт без конца */
    void *base = sbrk_raw(0);
    for (int i = 0; i < 200; i++) { void *x = malloc(1000); CHECK(x != 0); free(x); }
    CHECK(sbrk_raw(0) == base);
    char *c = calloc(100, 4); { int ok = c != 0; for (int i = 0; ok && i < 400; i++) if (c[i]) ok = 0; CHECK(ok); } free(c);
    CHECK(calloc((size_t)-1, 16) == 0);                       /* переполнение cnt*sz */
    CHECK(malloc(1UL << 40) == 0);                            /* такой кучи нет */

    if (fails) { printf("libc_test: ПРОВАЛЕНО %d\n", fails); return 1; }
    puts("libc_test: OK");
    return 0;
}
