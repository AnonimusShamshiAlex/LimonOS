/* Хост-клиент для tests/test_ftp.py: тот же kernel/ftp.c + tls.c + crypto.c, транспорт — сокеты Linux. */
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

/* ftp_host_test порт op(list|get) путь пользователь пароль sec(auto|tls|plain) [implicit] */
int main(int argc, char **argv) {
    if (argc < 7) return 64;
    rng_add_entropy("ftp-host-test", 13);
    struct ftp_ops ops = { NULL, d_dial, d_send, d_recv, d_close, d_say, NULL };
    struct ftp_req rq = { "localhost", 0x7F000001u, (uint16_t)atoi(argv[1]), argv[4], argv[5],
                          strcmp(argv[2], "get") == 0 ? FTP_GET : FTP_LIST, strcmp(argv[3], "-") ? argv[3] : NULL,
                          strcmp(argv[6], "tls") == 0 ? FTP_SEC_TLS : strcmp(argv[6], "plain") == 0 ? FTP_SEC_PLAIN : FTP_SEC_AUTO,
                          argc > 7 && strcmp(argv[7], "implicit") == 0 };
    struct ftp_result res;
    int r = ftp_run(&ops, &rq, &res);
    fprintf(stderr, "RESULT r=%d code=%d ctl_tls=%d data_tls=%d resumed=%d trunc=%d msg=%s\n", r, res.code, res.control_tls, res.data_tls, res.resumed, res.truncated, res.msg);
    if (r == 0) fwrite(res.data, 1, res.len, stdout);
    return r == 0 ? 0 : 1;
}
