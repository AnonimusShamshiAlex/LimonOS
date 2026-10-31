/* Проверка kernel/syscall.c: диспетчер системных вызовов на поддельной памяти процесса,
 * поддельной VFS и поддельной сети. Что НЕ проверяется на хосте: сам `int 0x80` (переход
 * ring3->ring0, возврат rax через iretq), настоящая VFS (vfs.c) и настоящий net.c — это
 * только QEMU/железо и их собственные тесты (vfs_host.c, net.c уже дружит с tcp_host.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "syscall.h"
#include "ring3.h"

static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %d: %s\n", __LINE__, #c); fails++; } }while(0)

/* "память процесса": одна область [base, base+len), всё остальное не отображено.
 * writable отмечает, можно ли ядру писать в эту область через copy_to_user (как PTE_WRITE). */
struct fake {
    uint64_t base; uint8_t *mem; size_t len; int writable;
    char out[8192]; size_t out_len;
    int permissive;
    int heap_fail_at;
    int heap_calls; uint64_t heap_va[64];

    /* поддельная VFS: один файл и одна папка, которые open() может найти */
    const char *file_path; const uint8_t *file_data; size_t file_len;
    const char *dir_path;
    int vfs_open_calls;

    /* поддельная сеть: connect() либо даёт готовый id, либо код ошибки tcp_connect */
    int connect_result;          /* что вернёт net_connect */
    int connect_calls, close_calls;
    char sent[4096]; size_t sent_len;    /* всё, что ушло через net_send */
    long send_result;            /* если >=0 — не используется (используется n); если <0 — именно это вернёт net_send */
    const char *recv_data; size_t recv_len, recv_pos;  /* что net_recv отдаёт по кусочкам */
    long recv_result_override;   /* если ненулевое — вернуть ровно это вместо данных (для проверки EOF/EAGAIN/ошибок) */
    int recv_use_override;
};
static int f_copy_from(void *c, void *dst, uint64_t uva, size_t n) {
    struct fake *f = c;
    if (f->permissive) { memset(dst, 'x', n); return 0; }
    if (uva < f->base || uva + n > f->base + f->len) return -1;
    memcpy(dst, f->mem + (uva - f->base), n);
    return 0;
}
static int f_copy_to(void *c, uint64_t uva, const void *src, size_t n) {
    struct fake *f = c;
    if (!f->writable) return -1;
    if (f->permissive) return 0;
    if (uva < f->base || uva + n > f->base + f->len) return -1;
    memcpy(f->mem + (uva - f->base), src, n);
    return 0;
}
static int f_heap(void *c, uint64_t uva) {
    struct fake *f = c;
    int k = f->heap_calls++;
    if (k == f->heap_fail_at) return -1;
    f->heap_va[k] = uva;
    return 0;
}
static void f_out(void *c, const char *b, size_t n) { struct fake *f = c; memcpy(f->out + f->out_len, b, n); f->out_len += n; }

static int f_vfs_open(void *c, const char *path, size_t pathlen, const uint8_t **data, size_t *len) {
    struct fake *f = c;
    f->vfs_open_calls++;
    (void)pathlen;
    if (f->dir_path && strcmp(path, f->dir_path) == 0) return -2;
    if (f->file_path && strcmp(path, f->file_path) == 0) { *data = f->file_data; *len = f->file_len; return 0; }
    return -1;
}
static int f_net_connect(void *c, uint32_t ip, uint16_t port) {
    struct fake *f = c; (void)ip; (void)port;
    f->connect_calls++;
    return f->connect_result;
}
static void f_net_close(void *c, int id) { struct fake *f = c; (void)id; f->close_calls++; }
static long f_net_send(void *c, int id, const void *buf, size_t n) {
    struct fake *f = c; (void)id;
    if (f->send_result < 0) return f->send_result;
    memcpy(f->sent + f->sent_len, buf, n);
    f->sent_len += n;
    return (long)n;
}
static long f_net_recv(void *c, int id, void *buf, size_t max) {
    struct fake *f = c; (void)id;
    if (f->recv_use_override) return f->recv_result_override;
    size_t avail = f->recv_len - f->recv_pos;
    size_t n = max < avail ? max : avail;
    if (n == 0) return 0;              /* кончились подготовленные данные -> «собеседник закрыл» в этих тестах */
    memcpy(buf, f->recv_data + f->recv_pos, n);
    f->recv_pos += n;
    return (long)n;
}

static struct sys_ops mk(struct fake *f) {
    struct sys_ops o = { f, f_copy_from, f_copy_to, f_heap, f_out, f_vfs_open, f_net_connect, f_net_close, f_net_send, f_net_recv };
    return o;
}
static void fake_init(struct fake *f, size_t len) {
    memset(f, 0, sizeof *f);
    f->base = USER_CODE_VA; f->len = len; f->mem = calloc(1, len ? len : 1); f->writable = 1;
    for (size_t i = 0; i < len; i++) f->mem[i] = (uint8_t)('a' + i % 26);
    f->heap_fail_at = -1;
    f->connect_result = 7;             /* произвольный "успешный" id по умолчанию */
}

int main(void) {
    struct fake f; struct sys_state st; struct sys_ops ops;

    /* ---- write (fd 1/2) — без изменений с Фазы 4 ---- */
    fake_init(&f, 8192); ops = mk(&f); sys_state_init(&st);
    CHECK(sys_dispatch(&st, &ops, SYS_write, 1, USER_CODE_VA, 5) == 5);
    CHECK(f.out_len == 5 && memcmp(f.out, "abcde", 5) == 0);
    CHECK(sys_dispatch(&st, &ops, SYS_write, 2, USER_CODE_VA + 1, 3) == 3 && memcmp(f.out + 5, "bcd", 3) == 0);
    CHECK(sys_dispatch(&st, &ops, SYS_write, 3, USER_CODE_VA, 5) == -E_BADF);     /* fd 3 ещё не открыт */
    CHECK(sys_dispatch(&st, &ops, SYS_write, 0, USER_CODE_VA, 5) == -E_BADF);
    size_t before = f.out_len;
    CHECK(sys_dispatch(&st, &ops, SYS_write, 1, USER_CODE_VA, 0) == 0 && f.out_len == before);
    f.out_len = 0;
    CHECK(sys_dispatch(&st, &ops, SYS_write, 1, USER_CODE_VA, 5000) == SYS_WRITE_MAX);
    CHECK(f.out_len == SYS_WRITE_MAX);
    f.out_len = 0;
    CHECK(sys_dispatch(&st, &ops, SYS_write, 1, KERNEL_TEXT_VA, 16) == -E_FAULT);
    CHECK(sys_dispatch(&st, &ops, SYS_write, 1, USER_VA_END, 1) == -E_FAULT);
    CHECK(sys_dispatch(&st, &ops, SYS_write, 1, 0x1000, 4) == -E_FAULT);
    CHECK(f.out_len == 0);
    free(f.mem);

    /* ---- brk — без изменений ---- */
    fake_init(&f, 16); ops = mk(&f); sys_state_init(&st);
    CHECK(sys_dispatch(&st, &ops, SYS_brk, 0, 0, 0) == (int64_t)USER_HEAP_VA);
    CHECK(sys_dispatch(&st, &ops, SYS_brk, USER_HEAP_VA + 5000, 0, 0) == (int64_t)(USER_HEAP_VA + 5000));
    CHECK(f.heap_calls == 2);
    free(f.mem);

    /* ---- read / exit / неизвестное ---- */
    fake_init(&f, 16); ops = mk(&f); sys_state_init(&st);
    CHECK(sys_dispatch(&st, &ops, SYS_read, 0, USER_CODE_VA, 10) == 0);
    CHECK(sys_dispatch(&st, &ops, SYS_read, 1, USER_CODE_VA, 10) == -E_BADF);     /* fd 1 не для чтения, и не открыт как файл/сокет */
    CHECK(sys_dispatch(&st, &ops, 9999, 0, 0, 0) == -E_NOSYS);
    CHECK(sys_dispatch(&st, &ops, SYS_exit, 0x1FF, 0, 0) == 0 && st.exited && st.exit_code == 0xFF);
    free(f.mem);

    /* ================= Фаза 6: open / read / lseek / close на файле ================= */
    {
        static const uint8_t content[] = "privet mir\n0123456789";
        fake_init(&f, 300); ops = mk(&f); sys_state_init(&st);
        f.file_path = "keys.txt"; f.file_data = content; f.file_len = sizeof content - 1;
        f.dir_path = "Desktop";
        strcpy((char *)f.mem, "keys.txt");       /* путь лежит в "памяти процесса" по USER_CODE_VA */

        int64_t fd = sys_dispatch(&st, &ops, SYS_open, USER_CODE_VA, 0, 0);
        CHECK(fd == FD_RESERVED);                 /* первый свободный дескриптор — 3 */
        CHECK(f.vfs_open_calls == 1);

        /* читаем по частям, позиция сдвигается */
        char rb[8];
        int64_t n1 = sys_dispatch(&st, &ops, SYS_read, (uint64_t)fd, USER_CODE_VA + 32, 5);
        CHECK(n1 == 5 && memcmp(f.mem + 32, "privet", 5) == 0);
        int64_t n2 = sys_dispatch(&st, &ops, SYS_read, (uint64_t)fd, USER_CODE_VA + 32, 100);   /* остаток файла одним куском */
        CHECK(n2 == (int64_t)(f.file_len - 5));
        int64_t n3 = sys_dispatch(&st, &ops, SYS_read, (uint64_t)fd, USER_CODE_VA + 32, 10);
        CHECK(n3 == 0);                            /* конец файла: 0, а не ошибка */
        (void)rb;

        /* lseek назад и перечитать */
        CHECK(sys_dispatch(&st, &ops, SYS_lseek, (uint64_t)fd, 0, 0) == 0);          /* SEEK_SET */
        int64_t n4 = sys_dispatch(&st, &ops, SYS_read, (uint64_t)fd, USER_CODE_VA + 32, 6);
        CHECK(n4 == 6 && memcmp(f.mem + 32, "privet", 6) == 0);
        CHECK(sys_dispatch(&st, &ops, SYS_lseek, (uint64_t)fd, 0, 2) == (int64_t)f.file_len);  /* SEEK_END */
        CHECK(sys_dispatch(&st, &ops, SYS_lseek, (uint64_t)fd, (uint64_t)(-3LL), 1) == (int64_t)f.file_len - 3); /* SEEK_CUR назад */
        CHECK(sys_dispatch(&st, &ops, SYS_lseek, (uint64_t)fd, (uint64_t)-1LL, 0) == -E_INVAL);     /* до нуля нельзя */
        CHECK(sys_dispatch(&st, &ops, SYS_lseek, (uint64_t)fd, (uint64_t)(f.file_len + 1), 0) == -E_INVAL); /* за конец нельзя */
        CHECK(sys_dispatch(&st, &ops, SYS_lseek, (uint64_t)fd, 0, 9) == -E_INVAL);                  /* плохой whence */

        /* на файл нельзя писать */
        CHECK(sys_dispatch(&st, &ops, SYS_write, (uint64_t)fd, USER_CODE_VA, 3) == -E_BADF);

        /* close и дальнейшее использование — EBADF; close(fd 0/1/2) — не ошибка, как в Linux */
        CHECK(sys_dispatch(&st, &ops, SYS_close, (uint64_t)fd, 0, 0) == 0);
        CHECK(sys_dispatch(&st, &ops, SYS_read, (uint64_t)fd, USER_CODE_VA, 1) == -E_BADF);
        CHECK(sys_dispatch(&st, &ops, SYS_close, (uint64_t)fd, 0, 0) == -E_BADF);     /* второй close того же fd */
        CHECK(sys_dispatch(&st, &ops, SYS_close, 1, 0, 0) == 0);

        /* не найден / это папка */
        strcpy((char *)f.mem, "net.txt");
        CHECK(sys_dispatch(&st, &ops, SYS_open, USER_CODE_VA, 0, 0) == -E_NOENT);
        strcpy((char *)f.mem, "Desktop");
        CHECK(sys_dispatch(&st, &ops, SYS_open, USER_CODE_VA, 0, 0) == -E_ISDIR);

        /* путь дальше SYS_PATH_MAX без NUL -> ENAMETOOLONG, память не переполняется */
        memset(f.mem, 'A', f.len);
        CHECK(sys_dispatch(&st, &ops, SYS_open, USER_CODE_VA, 0, 0) == -E_NAMETOOLONG);

        /* дескрипторы кончаются: MAX_FDS-FD_RESERVED файлов открываются, следующий — EMFILE */
        sys_state_init(&st);
        strcpy((char *)f.mem, "keys.txt");
        int opened = 0;
        for (int i = 0; i < MAX_FDS; i++) {
            int64_t r = sys_dispatch(&st, &ops, SYS_open, USER_CODE_VA, 0, 0);
            if (r < 0) { CHECK(r == -E_MFILE); break; }
            opened++;
        }
        CHECK(opened == MAX_FDS - FD_RESERVED);
        free(f.mem);
    }

    /* ================= Фаза 6: socket / connect / read / write на сокете ================= */
    {
        fake_init(&f, 64); ops = mk(&f); sys_state_init(&st);

        /* домен/тип, которые мы не поддерживаем */
        CHECK(sys_dispatch(&st, &ops, SYS_socket, 10 /* не AF_INET */, 1, 0) == -E_INVAL);
        CHECK(sys_dispatch(&st, &ops, SYS_socket, 2, 2 /* SOCK_DGRAM */, 0) == -E_INVAL);

        int64_t sfd = sys_dispatch(&st, &ops, SYS_socket, 2, 1, 0);
        CHECK(sfd == FD_RESERVED);

        /* до connect(): read/write на сокет -> ENOTCONN */
        CHECK(sys_dispatch(&st, &ops, SYS_write, (uint64_t)sfd, USER_CODE_VA, 3) == -E_NOTCONN);
        CHECK(sys_dispatch(&st, &ops, SYS_read, (uint64_t)sfd, USER_CODE_VA, 3) == -E_NOTCONN);

        /* sockaddr_in: family=2 (LE), port=8000 (BE), 10.0.2.15 (BE, как в net_parse_ip) */
        uint8_t *sa = f.mem;
        sa[0] = 2; sa[1] = 0;
        sa[2] = (8000 >> 8) & 0xFF; sa[3] = 8000 & 0xFF;
        sa[4] = 10; sa[5] = 0; sa[6] = 2; sa[7] = 15;
        memset(sa + 8, 0, 8);

        CHECK(sys_dispatch(&st, &ops, SYS_connect, (uint64_t)sfd, USER_CODE_VA, 15) == -E_INVAL);  /* addrlen < 16 */
        f.connect_result = 7;
        CHECK(sys_dispatch(&st, &ops, SYS_connect, (uint64_t)sfd, USER_CODE_VA, 16) == 0);
        CHECK(f.connect_calls == 1);
        CHECK(sys_dispatch(&st, &ops, SYS_connect, (uint64_t)sfd, USER_CODE_VA, 16) == -E_INVAL);  /* уже подключён */

        /* write -> net_send, данные реально дошли */
        memcpy(f.mem + 32, "GET /\r\n", 7);
        CHECK(sys_dispatch(&st, &ops, SYS_write, (uint64_t)sfd, USER_CODE_VA + 32, 7) == 7);
        CHECK(f.sent_len == 7 && memcmp(f.sent, "GET /\r\n", 7) == 0);

        /* read -> net_recv, данные реально пришли и скопировались процессу (через copy_to_user) */
        f.recv_data = "HTTP/1.0 200 OK\r\n"; f.recv_len = strlen(f.recv_data);
        int64_t rn = sys_dispatch(&st, &ops, SYS_read, (uint64_t)sfd, USER_CODE_VA + 32, 9);
        CHECK(rn == 9 && memcmp(f.mem + 32, "HTTP/1.0 ", 9) == 0);

        /* EAGAIN (нет данных, соединение живо) — не путается с EOF */
        f.recv_use_override = 1; f.recv_result_override = -E_AGAIN;
        CHECK(sys_dispatch(&st, &ops, SYS_read, (uint64_t)sfd, USER_CODE_VA + 32, 9) == -E_AGAIN);

        /* настоящий EOF: net_recv возвращает 0 */
        f.recv_result_override = 0;
        CHECK(sys_dispatch(&st, &ops, SYS_read, (uint64_t)sfd, USER_CODE_VA + 32, 9) == 0);

        /* обрыв соединения при отправке */
        f.send_result = -E_IO;
        CHECK(sys_dispatch(&st, &ops, SYS_write, (uint64_t)sfd, USER_CODE_VA + 32, 3) == -E_IO);

        /* close -> действительно закрывает соединение в сети, повторный close -> EBADF */
        CHECK(sys_dispatch(&st, &ops, SYS_close, (uint64_t)sfd, 0, 0) == 0 && f.close_calls == 1);
        CHECK(sys_dispatch(&st, &ops, SYS_close, (uint64_t)sfd, 0, 0) == -E_BADF);

        /* socket() без connect(), сразу close(): net_close НЕ вызывается (нечего закрывать) */
        fake_init(&f, 64); ops = mk(&f); sys_state_init(&st);
        int64_t sfd2 = sys_dispatch(&st, &ops, SYS_socket, 2, 1, 0);
        CHECK(sys_dispatch(&st, &ops, SYS_close, (uint64_t)sfd2, 0, 0) == 0 && f.close_calls == 0);

        /* connect возвращает коды tcp_connect -> нужный errno */
        fake_init(&f, 64); ops = mk(&f); sys_state_init(&st);
        memset(f.mem, 0, f.len); f.mem[0] = 2;
        int64_t sfd3 = sys_dispatch(&st, &ops, SYS_socket, 2, 1, 0);
        f.connect_result = -2; CHECK(sys_dispatch(&st, &ops, SYS_connect, (uint64_t)sfd3, USER_CODE_VA, 16) == -E_TIMEDOUT);
        f.connect_result = -3; CHECK(sys_dispatch(&st, &ops, SYS_connect, (uint64_t)sfd3, USER_CODE_VA, 16) == -E_CONNREFUSED);
        f.connect_result = -4; CHECK(sys_dispatch(&st, &ops, SYS_connect, (uint64_t)sfd3, USER_CODE_VA, 16) == -E_IO);

        free(f.mem);
    }

    /* ---- модель трёх ring3-программ из usermode.S — без изменений с Фазы 4 ---- */
    extern const uint8_t user_prog_hello_start[], user_prog_hello_end[];
    extern const uint8_t user_prog_efault_start[], user_prog_efault_end[];
    extern const uint8_t user_prog_nosys_start[], user_prog_nosys_end[];
    {
        struct { const uint8_t *s, *e; int n; } p[3] = {
            { user_prog_hello_start, user_prog_hello_end, 5 },
            { user_prog_efault_start, user_prog_efault_end, 2 },
            { user_prog_nosys_start, user_prog_nosys_end, 2 } };
        for (int i = 0; i < 3; i++) {
            int n = 0; for (const uint8_t *q = p[i].s; q + 1 < p[i].e; q++) if (q[0] == 0xCD && q[1] == 0x80) n++;
            CHECK(n == p[i].n);
        }
        const size_t ml = sizeof(USER_HELLO_MSG) - 1;
        CHECK((size_t)(user_prog_hello_end - user_prog_hello_start) > ml);
        CHECK(memcmp(user_prog_hello_start + ((size_t)(user_prog_hello_end - user_prog_hello_start) - ml), USER_HELLO_MSG, ml) == 0);
    }
    {
        size_t len = (size_t)(user_prog_hello_end - user_prog_hello_start);
        fake_init(&f, len); memcpy(f.mem, user_prog_hello_start, len); ops = mk(&f); sys_state_init(&st);
        const size_t ml = sizeof(USER_HELLO_MSG) - 1;
        int64_t r1 = sys_dispatch(&st, &ops, SYS_write, 1, USER_CODE_VA + (len - ml), ml);
        int64_t r2 = sys_dispatch(&st, &ops, SYS_brk, 0, 0, 0);
        int64_t r3 = sys_dispatch(&st, &ops, SYS_brk, (uint64_t)r2 + 0x2000, 0, 0);
        sys_dispatch(&st, &ops, SYS_exit, 42, 0, 0);
        CHECK(r1 == (int64_t)ml && r2 == (int64_t)USER_HEAP_VA && r3 == r2 + 0x2000);
        struct user_result r = { 0 }; r.exit_kind = USER_EXIT_SYSCALL; r.cs = SEL_UCODE; r.ss = SEL_UDATA;
        r.exit_code = st.exit_code; r.sys_calls = st.calls; r.out_len = f.out_len;
        f.out[f.out_len] = 0;
        CHECK(user_check_hello(&r, f.out));
        free(f.mem);
    }
    {
        fake_init(&f, 16); ops = mk(&f); sys_state_init(&st);
        int64_t e = sys_dispatch(&st, &ops, SYS_write, 1, KERNEL_TEXT_VA, 16);
        sys_dispatch(&st, &ops, SYS_exit, (uint64_t)-e, 0, 0);
        struct user_result r = { 0 }; r.exit_kind = USER_EXIT_SYSCALL; r.cs = SEL_UCODE; r.ss = SEL_UDATA;
        r.exit_code = st.exit_code; r.sys_calls = st.calls; r.out_len = f.out_len;
        CHECK(e == -E_FAULT && user_check_errno(&r, E_FAULT));
        free(f.mem);
    }

    if (fails) { printf("ПРОВАЛЕНО проверок: %d\n", fails); return 1; }
    printf("syscall host: все проверки прошли\n");
    return 0;
}
