#include "syscall.h"
#include "ring3.h"

void sys_state_init(struct sys_state *st) {
    st->brk_cur = USER_HEAP_VA;
    st->mapped_end = USER_HEAP_VA;
    st->exited = 0;
    st->exit_code = 0;
    st->calls = 0;
    for (int i = 0; i < MAX_FDS; i++) { st->fds[i].kind = FD_FREE; st->fds[i].tcp_id = -1; }
}

static int fd_alloc(struct sys_state *st) {
    for (int i = FD_RESERVED; i < MAX_FDS; i++) if (st->fds[i].kind == FD_FREE) return i;
    return -1;
}
static int fd_valid(uint64_t fd) { return fd >= FD_RESERVED && fd < MAX_FDS; }

static int64_t sys_write(struct sys_state *st, const struct sys_ops *ops, uint64_t fd, uint64_t buf, uint64_t len) {
    if (fd == 1 || fd == 2) {
        if (len == 0) return 0;
        if (len > SYS_WRITE_MAX) len = SYS_WRITE_MAX;
        /* Весь диапазон обязан лежать в пользовательской половине. Переполнения buf+len быть не может:
         * len уже урезан до 4 КиБ, а buf проверяется первым (< 2^47). */
        if (!user_addr_ok(buf) || !user_addr_ok(buf + len - 1)) return -E_FAULT;
        char tmp[256];
        uint64_t done = 0;
        while (done < len) {
            size_t n = (size_t)(len - done > sizeof tmp ? sizeof tmp : len - done);
            if (ops->copy_from_user(ops->ctx, tmp, buf + done, n) != 0) return done ? (int64_t)done : -E_FAULT;
            ops->out(ops->ctx, tmp, n);
            done += n;
        }
        return (int64_t)done;
    }
    /* Фаза 6: write() на fd из open()/socket(). Файлы у нас только для чтения. */
    if (!fd_valid(fd)) return -E_BADF;
    struct fd_entry *e = &st->fds[fd];
    if (e->kind == FD_FREE) return -E_BADF;
    if (e->kind == FD_FILE) return -E_BADF;           /* read-only VFS: писать в файл нельзя */
    if (e->tcp_id < 0) return -E_NOTCONN;              /* socket() был, connect() — не было */
    if (len == 0) return 0;
    if (len > SYS_WRITE_MAX) len = SYS_WRITE_MAX;
    if (!user_addr_ok(buf) || !user_addr_ok(buf + len - 1)) return -E_FAULT;
    char tmp[256];
    uint64_t done = 0;
    while (done < len) {
        size_t n = (size_t)(len - done > sizeof tmp ? sizeof tmp : len - done);
        if (ops->copy_from_user(ops->ctx, tmp, buf + done, n) != 0) return done ? (int64_t)done : -E_FAULT;
        long sent = ops->net_send(ops->ctx, e->tcp_id, tmp, n);     /* уже -errno или >=0 отправлено */
        if (sent < 0) return done ? (int64_t)done : sent;
        done += (uint64_t)sent;
        if ((size_t)sent < n) break;                                /* частичная отправка: как в Linux, не ждём дальше */
    }
    return (int64_t)done;
}

static int64_t sys_read(struct sys_state *st, const struct sys_ops *ops, uint64_t fd, uint64_t buf, uint64_t len) {
    if (fd == 0) return 0;                              /* stdin пока пуст: EOF */
    if (!fd_valid(fd)) return -E_BADF;
    struct fd_entry *e = &st->fds[fd];
    if (e->kind == FD_FREE) return -E_BADF;
    if (len == 0) return 0;
    if (len > SYS_WRITE_MAX) len = SYS_WRITE_MAX;
    if (!user_addr_ok(buf) || !user_addr_ok(buf + len - 1)) return -E_FAULT;

    if (e->kind == FD_FILE) {
        size_t avail = e->len - e->pos;                 /* pos <= len всегда (lseek это гарантирует) */
        size_t n = (size_t)len < avail ? (size_t)len : avail;
        if (n == 0) return 0;                            /* конец файла */
        if (ops->copy_to_user(ops->ctx, buf, e->data + e->pos, n) != 0) return -E_FAULT;
        e->pos += n;
        return (int64_t)n;
    }
    /* FD_SOCK */
    if (e->tcp_id < 0) return -E_NOTCONN;
    char tmp[256];
    size_t want = (size_t)len < sizeof tmp ? (size_t)len : sizeof tmp;
    long n = ops->net_recv(ops->ctx, e->tcp_id, tmp, want);   /* >0 данные, 0 EOF, <0 уже -errno */
    if (n <= 0) return n;
    if (ops->copy_to_user(ops->ctx, buf, tmp, (size_t)n) != 0) return -E_FAULT;
    return n;
}

/* open(): читает path из памяти процесса кусками по 64 байта (как sys_write — не считаем,
 * что у нас есть функция «скопировать строку произвольной длины»), ищет его в VFS (Memoria,
 * vfs.h) от корня и заводит дескриптор. Файл только для чтения: данные принадлежат VFS, а не
 * процессу, поэтому close() их не освобождает. */
static int64_t sys_dispatch_open(struct sys_state *st, const struct sys_ops *ops, uint64_t path_uva, uint64_t flags) {
    (void)flags;
    if (!user_addr_ok(path_uva)) return -E_FAULT;
    char path[SYS_PATH_MAX + 1];
    size_t n = 0;
    for (;;) {
        char chunk[64];
        if (!user_addr_ok(path_uva + n + sizeof chunk - 1)) return -E_FAULT;
        if (ops->copy_from_user(ops->ctx, chunk, path_uva + n, sizeof chunk) != 0) return -E_FAULT;
        for (size_t i = 0; i < sizeof chunk; i++) {
            if (n >= SYS_PATH_MAX) return -E_NAMETOOLONG;
            path[n++] = chunk[i];
            if (chunk[i] == 0) goto got_path2;
        }
    }
got_path2:
    if (n == 0 || path[0] == 0) return -E_NOENT;
    int fd = fd_alloc(st);
    if (fd < 0) return -E_MFILE;
    const uint8_t *data; size_t len;
    int r = ops->vfs_open(ops->ctx, path, n - 1, &data, &len);
    if (r == -1) return -E_NOENT;
    if (r == -2) return -E_ISDIR;
    if (r != 0)  return -E_IO;
    st->fds[fd].kind = FD_FILE; st->fds[fd].data = data; st->fds[fd].len = len; st->fds[fd].pos = 0;
    return fd;
}

static int64_t sys_close(struct sys_state *st, const struct sys_ops *ops, uint64_t fd) {
    if (fd == 0 || fd == 1 || fd == 2) return 0;        /* как в Linux: close() стандартных можно, это не ошибка */
    if (!fd_valid(fd)) return -E_BADF;
    struct fd_entry *e = &st->fds[fd];
    if (e->kind == FD_FREE) return -E_BADF;
    if (e->kind == FD_SOCK && e->tcp_id >= 0) ops->net_close(ops->ctx, e->tcp_id);
    e->kind = FD_FREE; e->tcp_id = -1;
    return 0;
}

static int64_t sys_lseek(struct sys_state *st, uint64_t fd, int64_t off, uint64_t whence) {
    if (!fd_valid(fd)) return -E_BADF;
    struct fd_entry *e = &st->fds[fd];
    if (e->kind != FD_FILE) return -E_BADF;              /* сокеты не перематываются */
    int64_t base;
    switch (whence) {
    case 0: base = 0; break;                              /* SEEK_SET */
    case 1: base = (int64_t)e->pos; break;                /* SEEK_CUR */
    case 2: base = (int64_t)e->len; break;                /* SEEK_END */
    default: return -E_INVAL;
    }
    int64_t np = base + off;
    if (np < 0 || (uint64_t)np > e->len) return -E_INVAL;
    e->pos = (size_t)np;
    return np;
}

static int64_t sys_socket(struct sys_state *st, uint64_t domain, uint64_t type) {
    if (domain != 2 /* AF_INET */ || (type & 0xFF) != 1 /* SOCK_STREAM */) return -E_INVAL;
    int fd = fd_alloc(st);
    if (fd < 0) return -E_MFILE;
    st->fds[fd].kind = FD_SOCK; st->fds[fd].tcp_id = -1;
    return fd;
}

static int64_t sys_connect(struct sys_state *st, const struct sys_ops *ops, uint64_t fd, uint64_t addr_uva, uint64_t addrlen) {
    if (!fd_valid(fd)) return -E_BADF;
    struct fd_entry *e = &st->fds[fd];
    if (e->kind != FD_SOCK) return -E_BADF;
    if (e->tcp_id >= 0) return -E_INVAL;                  /* уже подключён */
    if (addrlen < 16) return -E_INVAL;
    if (!user_addr_ok(addr_uva) || !user_addr_ok(addr_uva + 15)) return -E_FAULT;
    uint8_t raw[16];
    if (ops->copy_from_user(ops->ctx, raw, addr_uva, 16) != 0) return -E_FAULT;
    /* struct sockaddr_in как в POSIX: [0:2)=family (LE), [2:4)=port (BE), [4:8)=addr (BE), остальное — ноль. */
    uint16_t family = (uint16_t)(raw[0] | (raw[1] << 8));
    if (family != 2) return -E_INVAL;
    uint16_t port = (uint16_t)((raw[2] << 8) | raw[3]);
    uint32_t ip = ((uint32_t)raw[4] << 24) | ((uint32_t)raw[5] << 16) | ((uint32_t)raw[6] << 8) | raw[7];
    int id = ops->net_connect(ops->ctx, ip, port);
    if (id == -2) return -E_TIMEDOUT;
    if (id == -3) return -E_CONNREFUSED;
    if (id < 0)   return -E_IO;
    e->tcp_id = id;
    return 0;
}

static int64_t sys_brk(struct sys_state *st, const struct sys_ops *ops, uint64_t want) {
    /* Linux: brk(0) и любая неудача возвращают ТЕКУЩИЙ brk, а не ошибку */
    if (want == 0 || want < USER_HEAP_VA || want > USER_HEAP_VA + USER_HEAP_MAX) return (int64_t)st->brk_cur;
    uint64_t need = (want + 0xFFF) & ~0xFFFULL;
    while (st->mapped_end < need) {
        if (ops->map_heap_page(ops->ctx, st->mapped_end) != 0) return (int64_t)st->brk_cur;
        st->mapped_end += 0x1000;
    }
    st->brk_cur = want;
    return (int64_t)st->brk_cur;
}

int64_t sys_dispatch(struct sys_state *st, const struct sys_ops *ops,
                     uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    st->calls++;
    switch (nr) {
    case SYS_read:    return sys_read(st, ops, a1, a2, a3);
    case SYS_write:   return sys_write(st, ops, a1, a2, a3);
    case SYS_open:    return sys_dispatch_open(st, ops, a1, a2);
    case SYS_close:   return sys_close(st, ops, a1);
    case SYS_lseek:   return sys_lseek(st, a1, (int64_t)a2, a3);
    case SYS_brk:     return sys_brk(st, ops, a1);
    case SYS_socket:  return sys_socket(st, a1, a2);
    case SYS_connect: return sys_connect(st, ops, a1, a2, a3);
    case SYS_exit:    st->exited = 1; st->exit_code = (int64_t)(a1 & 0xFF); return 0;
    default:          return -E_NOSYS;
    }
}
