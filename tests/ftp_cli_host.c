/* Хост-харнесс для tests/test_ftp.py: порядок «как в ftp.exe» — ftp_open (подключение + баннер) НИЧЕГО не логинит;
 * логин/пароль идут только по ftp_user/ftp_pass; неверный пароль не рвёт соединение; после входа — ls/dir/rmdir/rename/cdup. */
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
static int auth_cmds;                       /* сколько раз клиент сам отправил USER/PASS */
static void d_say(void *ud, int kind, const char *t) {
    (void)ud; fprintf(stderr, "[%d] %s\n", kind, t);
    if (kind == FTP_SAY_CMD && (!strncmp(t, "USER", 4) || !strncmp(t, "PASS", 4))) auth_cmds++;
}

#define CHECK(name, cond) do { \
    if (!(cond)) { printf("SESSION FAIL %s\n", name); fflush(stdout); return 1; } \
} while (0)


/* ftp_cli_host порт пользователь верный_пароль */
int main(int argc, char **argv) {
    if (argc < 4) return 64;
    rng_add_entropy("ftp-cli-host-test", 17);
    struct ftp_ops ops = { NULL, d_dial, d_send, d_recv, d_close, d_say, NULL };
    struct ftp_req rq = { "localhost", 0x7F000001u, (uint16_t)atoi(argv[1]), "", "", FTP_LIST, NULL, FTP_SEC_AUTO, 0 };
    struct ftp_session fs;
    struct ftp_result res;

    int r = ftp_open(&ops, &rq, &fs, &res);
    CHECK("open", r == FTP_OK && fs.open);
    CHECK("open сам НЕ логинится (USER/PASS не отправлены)", auth_cmds == 0 && !fs.logged);

    /* неверный пароль: ошибка, но соединение живо и можно повторить */
    r = ftp_user(&fs, argv[2], &res);
    CHECK("user -> нужен пароль", r == FTP_NEED_PASS);
    r = ftp_pass(&fs, "wrong", &res);
    CHECK("неверный пароль -> FTP_E_LOGIN, код 530", r == FTP_E_LOGIN && res.code == 530);
    CHECK("после неверного пароля соединение всё ещё открыто", fs.open && !fs.logged);

    r = ftp_user(&fs, argv[2], &res);
    CHECK("user ещё раз", r == FTP_NEED_PASS);
    r = ftp_pass(&fs, argv[3], &res);
    CHECK("верный пароль", r == FTP_OK && fs.logged);

    r = ftp_nlst(&fs, NULL, &res);
    CHECK("ls (NLST): только имена", r == FTP_OK && res.len > 0 && memcmp(res.data, "hello.txt", 9) == 0);
    kfree(res.data);
    r = ftp_list(&fs, NULL, &res);
    CHECK("dir (LIST): подробно", r == FTP_OK && res.len > 0 && strstr((char *)res.data, "-rw-r--r--") != NULL);
    kfree(res.data);

    r = ftp_cdup(&fs, &res);          CHECK("cdup", r == FTP_OK);
    r = ftp_rmdir(&fs, "old", &res);  CHECK("rmdir", r == FTP_OK);
    r = ftp_rename(&fs, "a.txt", "b.txt", &res); CHECK("rename (RNFR+RNTO)", r == FTP_OK);
    r = ftp_quote(&fs, "PWD", &res);  CHECK("quote PWD", r == FTP_OK && res.code == 257);

    fs.type = 'A';
    r = ftp_get(&fs, "/hello.txt", &res);
    CHECK("get в текстовом режиме", r == FTP_OK && res.len == 15);
    kfree(res.data);

    ftp_close(&fs);
    CHECK("close", fs.open == 0);
    printf("CLI OK\n");
    return 0;
}
