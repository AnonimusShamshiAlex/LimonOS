/* Своя мини-библиотека: компилятор сам вставляет вызовы memcpy/memset,
 * а стандартной библиотеки у нас нет. */
#include "kstring.h"

void *memcpy(void *dest, const void *src, size_t n) {
    void *ret = dest;
    __asm__ volatile ("rep movsb" : "+D"(dest), "+S"(src), "+c"(n) :: "memory");
    return ret;
}

void *memset(void *s, int c, size_t n) {
    void *ret = s;
    __asm__ volatile ("rep stosb" : "+D"(s), "+c"(n) : "a"(c) : "memory");
    return ret;
}

void *memmove(void *dest, const void *src, size_t n) {
    if (dest == src || n == 0) return dest;
    if ((uintptr_t)dest < (uintptr_t)src || (uintptr_t)dest >= (uintptr_t)src + n)
        return memcpy(dest, src, n);
    void *ret = dest;
    uint8_t *d = (uint8_t *)dest + n - 1;
    const uint8_t *s = (const uint8_t *)src + n - 1;
    __asm__ volatile ("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return ret;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    for (size_t i = 0; i < n; i++)
        if (p[i] != q[i]) return p[i] < q[i] ? -1 : 1;
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (uint8_t)*a - (uint8_t)*b : 0;
}

char *strchr(const char *s, int c) {
    while (*s) { if ((unsigned char)*s == (unsigned char)c) return (char *)s; s++; }
    return (c == 0) ? (char *)s : NULL;
}
