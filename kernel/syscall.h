#ifndef LIMON_SYSCALL_H
#define LIMON_SYSCALL_H
#include <stdint.h>
#include <stddef.h>

/*
 * Фаза 4 плана (docs/USERSPACE_PLAN.md): системные вызовы.
 *
 * Номера и соглашение — как в Linux x86_64 (rax = номер, аргументы rdi/rsi/rdx,
 * результат в rax, ошибка = -errno), чтобы позже тот же код пережил переход на
 * инструкцию `syscall` и портирование настоящих программ. Пока вход — `int 0x80`
 * (DPL=3 шлюз в IDT): проще, чем MSR для syscall/sysret.
 *
 * kernel/syscall.c — чистая логика: доступ к памяти процесса, к выводу и к выделению
 * страниц идёт через sys_ops, поэтому она проверяется на хосте (tests/syscall_host.c).
 *
 * Фаза 6: открытие файлов (только чтение, через уже готовую VFS/Memoria) и TCP-сокеты
 * поверх kernel/net.c. Оба — через ту же дырку int 0x80, номера настоящие линуксовые.
 */
#define SYS_read    0
#define SYS_write   1
#define SYS_open    2
#define SYS_close   3
#define SYS_lseek   8
#define SYS_brk     12
#define SYS_socket  41
#define SYS_connect 42
#define SYS_exit    60

/* errno — настоящие линуксовые номера (errno.h), не придуманные */
#define E_PERM         1
#define E_NOENT        2
#define E_IO           5
#define E_BADF         9
#define E_AGAIN       11   /* сокет: за время ожидания ничего не пришло, но соединение живо — не EOF */
#define E_NOMEM       12
#define E_FAULT       14
#define E_ISDIR       21
#define E_INVAL       22
#define E_MFILE       24   /* у процесса кончились дескрипторы (MAX_FDS) */
#define E_NAMETOOLONG 36
#define E_NOSYS       38
#define E_NOTCONN    107   /* connect() ещё не вызывали, а уже read/write/close тянут данные */
#define E_TIMEDOUT   110
#define E_CONNREFUSED 111

#define USER_HEAP_VA      0x0000000010000000ULL   /* начало кучи процесса (brk) */
#define USER_HEAP_MAX     (16ULL * 4096)          /* предел: 64 КиБ — пока без pmm_free */
#define SYS_WRITE_MAX     4096                    /* за один write/read принимаем не больше */
#define SYS_PATH_MAX       255                    /* путь в open(): длиннее — ENAMETOOLONG */

/* ---- Дескрипторы процесса: 0/1/2 — стандартные (stdin всегда пуст, stdout/stderr — вывод
 * программы), 3..MAX_FDS-1 — то, что процесс сам открыл через open()/socket(). ---- */
#define FD_RESERVED  3
#define MAX_FDS      8

enum fd_kind { FD_FREE = 0, FD_FILE = 1, FD_SOCK = 2 };

struct fd_entry {
    int kind;                 /* enum fd_kind */
    const uint8_t *data;      /* FD_FILE: содержимое (его хранит VFS, процесс НЕ владеет и не освобождает) */
    size_t len, pos;          /* FD_FILE: размер файла и текущая позиция чтения */
    int tcp_id;                /* FD_SOCK: id в net.c; -1, если socket() был, а connect() — ещё нет */
};

struct sys_ops {
    void *ctx;
    /* копирует n байт ИЗ памяти процесса; 0 — успех, -1 — адрес не отображён/не USER */
    int  (*copy_from_user)(void *ctx, void *dst, uint64_t uva, size_t n);
    /* копирует n байт В память процесса; 0 — успех, -1 — адрес не отображён/не USER/не для записи */
    int  (*copy_to_user)(void *ctx, uint64_t uva, const void *src, size_t n);
    /* отображает ОДНУ новую обнулённую страницу кучи по uva (выровнен); 0 — успех */
    int  (*map_heap_page)(void *ctx, uint64_t uva);
    /* вывод процесса (fd 1 и 2) */
    void (*out)(void *ctx, const char *buf, size_t n);
    /* open(): path — NUL-строка. 0 — успех (data/len заполнены, живут пока не закрыт процесс),
     * -1 — нет такого файла, -2 — это папка. Процесс НЕ освобождает data — им владеет VFS. */
    int  (*vfs_open)(void *ctx, const char *path, size_t pathlen, const uint8_t **out_data, size_t *out_len);
    /* TCP: ip/port как у tcp_connect (net.h). Возвращает id>=0, или тот же код ошибки, что и tcp_connect
     * (-2 не отвечает, -3 порт закрыт, -4 нет памяти), чтобы sys_connect сам перевёл его в errno. */
    int  (*net_connect)(void *ctx, uint32_t ip, uint16_t port);
    void (*net_close)(void *ctx, int tcp_id);
    /* >=0 — уже готовый -errno (E_IO и т.п.), это НЕ «сырое» число от net.c — ops сам его переводит,
     * потому что только ops знает про коды tcp_send/tcp_recv. 0 от recv значит «собеседник закрыл» (EOF). */
    long (*net_send)(void *ctx, int tcp_id, const void *buf, size_t n);
    long (*net_recv)(void *ctx, int tcp_id, void *buf, size_t max);
};

struct sys_state {
    uint64_t brk_cur;        /* текущий brk */
    uint64_t mapped_end;     /* куча отображена до этого адреса (кратен странице) */
    int      exited;
    int64_t  exit_code;
    unsigned calls;
    struct fd_entry fds[MAX_FDS];
};

void    sys_state_init(struct sys_state *st);
int64_t sys_dispatch(struct sys_state *st, const struct sys_ops *ops,
                     uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3);

#endif
