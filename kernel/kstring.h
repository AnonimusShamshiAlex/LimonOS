#ifndef LIMON_KSTRING_H
#define LIMON_KSTRING_H
#include <stddef.h>
#include <stdint.h>

void  *memcpy(void *dest, const void *src, size_t n);
void  *memset(void *s, int c, size_t n);
void  *memmove(void *dest, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
char  *strchr(const char *s, int c);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);

/* Затирает секретные данные (ключи) так, чтобы компилятор не выкинул запись. */
static inline void secure_zero(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

/* Ищет первое вхождение needle в [hay, hayend); NULL, если не нашёл. Не требует нулевого байта на конце диапазона. */
static inline const char *strstr_n(const char *hay, const char *hayend, const char *needle) {
    size_t nl = strlen(needle);
    if (!nl) return hay;
    for (const char *p = hay; p + nl <= hayend; p++) if (memcmp(p, needle, nl) == 0) return p;
    return NULL;
}

/* Декодирует %XX и '+' в диапазоне [s, send); пишет не больше cap-1 байт и всегда завершает нулём. */
static inline void urldecode_n(const char *s, const char *send, char *out, size_t cap) {
    size_t o = 0;
    while (s < send && o + 1 < cap) {
        if (*s == '%' && s + 2 < send) {
            int hi = s[1], lo = s[2];
            int v = -1;
            if (hi >= '0' && hi <= '9') v = (hi - '0') << 4; else if (hi >= 'a' && hi <= 'f') v = (hi - 'a' + 10) << 4; else if (hi >= 'A' && hi <= 'F') v = (hi - 'A' + 10) << 4;
            if (v >= 0) {
                int v2 = -1;
                if (lo >= '0' && lo <= '9') v2 = lo - '0'; else if (lo >= 'a' && lo <= 'f') v2 = lo - 'a' + 10; else if (lo >= 'A' && lo <= 'F') v2 = lo - 'A' + 10;
                if (v2 >= 0) { out[o++] = (char)(v | v2); s += 3; continue; }
            }
            out[o++] = *s++;
        } else if (*s == '+') { out[o++] = ' '; s++; }
        else out[o++] = *s++;
    }
    out[o] = 0;
}

/* Дописывает src в конец dst (dst — буфер размера cap, всегда с нулевым байтом на выходе). */
static inline void strncat_s(char *dst, const char *src, size_t cap) {
    size_t dl = strlen(dst);
    if (dl + 1 >= cap) return;
    size_t room = cap - dl - 1, sl = strlen(src);
    if (sl > room) sl = room;
    memcpy(dst + dl, src, sl);
    dst[dl + sl] = 0;
}

/* ---- UTF-8 ---- */

/* Читает один символ UTF-8 и сдвигает указатель. Вызывать только если **ps != 0 */
static inline uint32_t utf8_next(const char **ps) {
    const uint8_t *s = (const uint8_t *)*ps;
    uint32_t c = *s++;
    if (c < 0x80) {
    } else if ((c & 0xE0) == 0xC0 && (s[0] & 0xC0) == 0x80) {
        c = ((c & 0x1F) << 6) | (s[0] & 0x3F); s += 1;
    } else if ((c & 0xF0) == 0xE0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80) {
        c = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F); s += 2;
    } else if ((c & 0xF8) == 0xF0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        c = ((c & 0x07) << 18) | ((s[0] & 0x3F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); s += 3;
    } else {
        c = '?';
    }
    *ps = (const char *)s;
    return c;
}

/* Кодирует символ в UTF-8, возвращает число байт (1..4) */
static inline int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Сколько символов (а не байт) в строке UTF-8 */
static inline int utf8_len(const char *s) {
    int n = 0;
    while (*s) { utf8_next(&s); n++; }
    return n;
}

#endif
