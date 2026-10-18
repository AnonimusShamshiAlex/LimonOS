#ifndef LIMON_KSTRING_H
#define LIMON_KSTRING_H
#include <stddef.h>
#include <stdint.h>

void  *memcpy(void *dest, const void *src, size_t n);
void  *memset(void *s, int c, size_t n);
void  *memmove(void *dest, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);

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
