#ifndef LIMON_LIBC_H
#define LIMON_LIBC_H
/*
 * Мини-libc LimonOS (Фаза 5 плана, docs/USERSPACE_PLAN.md).
 * Это НЕ glibc и не musl: только то, что нужно, чтобы собирать и запускать простые
 * программы поверх системных вызовов Фазы 4 (read/write/brk/exit). Названия и
 * поведение — как в обычной C-библиотеке, чтобы программы переносились без правок.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* ---- системные вызовы (номера как в Linux x86_64) ---- */
#define SYS_read    0
#define SYS_write   1
#define SYS_open    2
#define SYS_close   3
#define SYS_lseek   8
#define SYS_brk     12
#define SYS_socket  41
#define SYS_connect 42
#define SYS_exit    60

/* lseek: откуда отсчитывать */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* socket(): LimonOS понимает только это сочетание (TCP/IPv4) */
#define AF_INET     2
#define SOCK_STREAM 1

/* ---- sockaddr_in как в POSIX <netinet/in.h>: 16 байт, те же смещения, что ждёт ядро.
 * sin_port и sin_addr — в сетевом порядке байт (старший байт первый); так же, как
 * у настоящего Linux, поэтому обычный htons()/написанный руками адрес работают одинаково. */
struct in_addr { uint32_t s_addr; };
struct sockaddr_in {
    uint16_t sin_family;      /* AF_INET */
    uint16_t sin_port;        /* сетевой порядок байт: htons(порт) */
    struct in_addr sin_addr;  /* сетевой порядок байт: a<<24|b<<16|c<<8|d через htonl, или вручную */
    uint8_t  sin_zero[8];
};
static inline uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint32_t ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
}

long __syscall3(long nr, long a1, long a2, long a3);
long write(int fd, const void *buf, size_t n);
long read(int fd, void *buf, size_t n);
int   open(const char *path, int flags, int mode);
int   close(int fd);
long  lseek(int fd, long offset, int whence);
int   socket(int domain, int type, int proto);
int   connect(int fd, const struct sockaddr_in *addr, long addrlen);
void *sbrk_raw(void *want);          /* brk(2): возвращает текущий/новый brk */
void _exit(int code) __attribute__((noreturn));
void exit(int code) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

/* ---- string.h ---- */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
char  *strchr(const char *s, int c);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

/* ---- stdlib.h ---- */
void *malloc(size_t n);
void *calloc(size_t cnt, size_t sz);
void *realloc(void *p, size_t n);
void  free(void *p);
int   atoi(const char *s);
int   abs(int x);

/* ---- stdio.h (без FILE: только стандартный вывод) ---- */
int vsnprintf(char *out, size_t cap, const char *fmt, va_list ap);
int snprintf(char *out, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int puts(const char *s);
int putchar(int c);

extern char **environ;
int main(int argc, char **argv);

#endif
