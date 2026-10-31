/* Хост-харнесс для tests/test_ftp.py: проверяет ЖИВУЮ сессию (ftp_connect + несколько команд
 * на ОДНОМ открытом соединении + ftp_close), а не разовый ftp_run. Это и есть то, что раньше
 * было сломано — клиент заходил по логину/паролю, но не оставался «внутри» для ls/cd/get/put.
 * Печатает одну строку "SESSION <OK|FAIL шаг>" в stdout. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "ftp.h"
#include "crypto.h"

void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }

struct sk { int fd; };
static void *d_dial(void *ud, uint32_t ip, uint16_t port, int to, int *err) {
    (void)ud; (void)to;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(ip) };
    if (connect(fd, (void *)&a, sizeof a)) { *err = -3; close(fd); return NULL; }
    struct sk *s = malloc(sizeof *s); s->fd = fd; return s;
}
static int d_send(void *h, const void *d, size_t n) {
    int fd = ((struct sk *)h)->fd; const char *p = d;
    while (n) { ssize_t k = send(fd, p, n, MSG_NOSIGNAL); if (k <= 0) return -1; p += k; n -= (size_t)k; }
    return 0;
}
static int d_recv(void *h, void *b, size_t max, int to) {
    int fd = ((struct sk *)h)->fd;
    struct pollfd pf = { fd, POLLIN, 0 };
    int r = poll(&pf, 1, to);
    if (r == 0) return 0;
    if (r < 0) return -1;
    ssize_t k = recv(fd, b, max, 0);
    return k > 0 ? (int)k : -1;
}
static void d_close(void *h) { close(((struct sk *)h)->fd); free(h); }
static void d_say(void *ud, int kind, const char *t) { (void)ud; fprintf(stderr, "[%d] %s\n", kind, t); }

#define CHECK(name, cond) do { \
    if (!(cond)) { printf("SESSION FAIL %s\n", name); fflush(stdout); return 1; } \
} while (0)

/* ftp_session_host порт пользователь пароль */
int main(int argc, char **argv) {
    if (argc < 4) return 64;
    rng_add_entropy("ftp-session-host-test", 21);
    struct ftp_ops ops = { NULL, d_dial, d_send, d_recv, d_close, d_say, NULL };
    struct ftp_req rq = { "localhost", 0x7F000001u, (uint16_t)atoi(argv[1]), argv[2], argv[3],
                           FTP_LIST, NULL, FTP_SEC_AUTO, 0 };
    struct ftp_session fs;
    struct ftp_result res;

    int r = ftp_connect(&ops, &rq, &fs, &res);
    CHECK("connect", r == FTP_OK && fs.open);

    /* два LIST подряд на ОДНОМ и том же соединении — не было бы смысла без постоянной сессии */
    r = ftp_list(&fs, NULL, &res);
    CHECK("list#1", r == FTP_OK && res.len > 0);
    kfree(res.data);
    r = ftp_list(&fs, NULL, &res);
    CHECK("list#2 (то же соединение)", r == FTP_OK && res.len > 0);
    kfree(res.data);

    char cur[256];
    r = ftp_pwd(&fs, cur, sizeof cur, &res);
    CHECK("pwd", r == FTP_OK && strstr(cur, "/current") != NULL);

    r = ftp_cwd(&fs, "/missing", &res);
    CHECK("cwd на несуществующую папку -> ошибка", r != FTP_OK && res.code == 550);

    r = ftp_cwd(&fs, "/ok", &res);
    CHECK("cwd -> успех", r == FTP_OK);

    r = ftp_mkdir(&fs, "newdir", &res);
    CHECK("mkdir", r == FTP_OK);

    r = ftp_rm(&fs, "newdir", &res);
    CHECK("rm", r == FTP_OK);

    static const uint8_t payload[] = "LimonOS ftp put: proверка\n";
    r = ftp_put(&fs, "/put_test.txt", payload, sizeof payload - 1, &res);
    CHECK("put", r == FTP_OK);

    /* соединение всё ещё то же самое: ещё один list после put */
    r = ftp_list(&fs, NULL, &res);
    CHECK("list#3 (после put, то же соединение)", r == FTP_OK && res.len > 0);
    kfree(res.data);

    ftp_close(&fs);
    CHECK("close", fs.open == 0);

    printf("SESSION OK\n");
    return 0;
}
