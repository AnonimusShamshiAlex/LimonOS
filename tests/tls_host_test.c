/* Хост-тест TLS LimonOS: тот же tls.c/crypto.c, но транспорт — обычный сокет Linux.
   Запуск:  tests/run_tls_test.sh   (поднимает настоящий OpenSSL-сервер и качает файл) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "tls.h"
#include "crypto.h"

void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }

static int sk_send(void *ctx, const void *d, size_t n) {
    int fd = *(int *)ctx; const char *p = d;
    while (n) { ssize_t k = send(fd, p, n, 0); if (k <= 0) return -1; p += k; n -= (size_t)k; }
    return 0;
}
static int sk_recv(void *ctx, void *b, size_t max, int to) {
    int fd = *(int *)ctx;
    struct pollfd pf = { fd, POLLIN, 0 };
    int r = poll(&pf, 1, to);
    if (r == 0) return 0;
    if (r < 0) return -1;
    ssize_t k = recv(fd, b, max, 0);
    return k > 0 ? (int)k : -1;
}

static int connect_local(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (void *)&a, sizeof a)) { perror("connect"); exit(2); }
    return fd;
}

/* tls_host_test порт путь [only12] [resume] — resume: два соединения подряд, второе должно возобновить сессию */
int main(int argc, char **argv) {
    int port = atoi(argv[1]); const char *path = argv[2];
    int flags = (argc > 3 && strcmp(argv[3], "only12") == 0) ? TLS_F_ONLY12 : 0;
    int want_resume = argc > 4 && strcmp(argv[4], "resume") == 0;
    rng_add_entropy("host-test", 9);
    struct tls_session sess; memset(&sess, 0, sizeof sess);
    for (int round = 0; round < (want_resume ? 2 : 1); round++) {
        int fd = connect_local(port);
        struct tls_io io = { sk_send, sk_recv, &fd };
        int err;
        struct tls *t = tls_open_ex(&io, "localhost", flags, round ? &sess : NULL, &err);
        if (!t) { fprintf(stderr, "ОШИБКА рукопожатия: %d (%s)\n", err, tls_strerror(err)); return 3; }
        fprintf(stderr, "рукопожатие OK, набор: %s, версия: %d, возобновлена: %d\n", tls_info(t)->cipher, tls_info(t)->version, tls_info(t)->resumed);
        if (want_resume && round == 0) {
            if (tls_session_save(t, &sess)) { fprintf(stderr, "сессию сохранить нельзя\n"); return 6; }
        }
        if (want_resume && round == 1 && !tls_info(t)->resumed) { fprintf(stderr, "ожидали возобновление\n"); return 7; }
        char req[256];
        int rn = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", path);
        if (tls_write(t, req, (size_t)rn)) { fprintf(stderr, "write fail\n"); return 4; }
        static char buf[70000]; long total = 0;
        for (;;) {
            int n = tls_read(t, buf, sizeof buf, 5000);
            if (n == 0) { fprintf(stderr, "таймаут чтения\n"); return 5; }
            if (n < 0) { if (n != TLS_EOF) fprintf(stderr, "конец с ошибкой %d (%s)\n", n, tls_strerror(n)); break; }
            if (round == (want_resume ? 1 : 0)) fwrite(buf, 1, (size_t)n, stdout);
            total += n;
        }
        fprintf(stderr, "получено %ld байт\n", total);
        tls_close(t);
        close(fd);
    }
    return 0;
}
