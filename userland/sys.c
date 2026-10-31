#include "libc.h"

/* Вход в ядро: в LimonOS — int 0x80 (Фаза 4). Для проверки libc на обычном Linux
 * (tests/run_host_tests.sh) та же библиотека собирается с -DLIMON_HOST_SYSCALL и
 * использует `syscall`: номера read/write/brk/exit у Linux такие же. */
long __syscall3(long nr, long a1, long a2, long a3) {
    long ret;
#ifdef LIMON_HOST_SYSCALL
    __asm__ volatile ("syscall" : "=a"(ret) : "0"(nr), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
#else
    __asm__ volatile ("int $0x80" : "=a"(ret) : "0"(nr), "D"(a1), "S"(a2), "d"(a3) : "memory");
#endif
    return ret;
}

static long fix(long r) { return r; }   /* errno пока нет: отрицательный результат возвращается как есть */
long write(int fd, const void *buf, size_t n) { return fix(__syscall3(SYS_write, fd, (long)buf, (long)n)); }
long read(int fd, void *buf, size_t n)  { return fix(__syscall3(SYS_read, fd, (long)buf, (long)n)); }
int  open(const char *path, int flags, int mode) { (void)mode; return (int)fix(__syscall3(SYS_open, (long)path, flags, 0)); }
int  close(int fd) { return (int)fix(__syscall3(SYS_close, fd, 0, 0)); }
long lseek(int fd, long offset, int whence) { return fix(__syscall3(SYS_lseek, fd, offset, whence)); }
int  socket(int domain, int type, int proto) { return (int)fix(__syscall3(SYS_socket, domain, type, proto)); }
int  connect(int fd, const struct sockaddr_in *addr, long addrlen) { return (int)fix(__syscall3(SYS_connect, fd, (long)addr, addrlen)); }
void *sbrk_raw(void *want) { return (void *)__syscall3(SYS_brk, (long)want, 0, 0); }
void _exit(int code) { for (;;) __syscall3(SYS_exit, code, 0, 0); }

char **environ;
void exit(int code) { _exit(code); }
void abort(void) { _exit(134); }

void __libc_start(int argc, char **argv, char **envp) {
    environ = envp;
    exit(main(argc, argv));
}
