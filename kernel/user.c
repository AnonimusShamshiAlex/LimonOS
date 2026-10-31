#include "user.h"
#include "mem.h"
#include "vmm.h"
#include "timer.h"
#include "kstring.h"
#include "syscall.h"
#include "elf.h"
#include "exec.h"
#include "vfs.h"
#include "net.h"

/* Программы из usermode.S */
extern const uint8_t user_prog_hlt_start[], user_prog_hlt_end[];
extern const uint8_t user_prog_pf_start[], user_prog_pf_end[];
extern const uint8_t user_prog_spin_start[], user_prog_spin_end[];
extern const uint8_t user_prog_hello_start[], user_prog_hello_end[];
extern const uint8_t user_prog_efault_start[], user_prog_efault_end[];
extern const uint8_t user_prog_nosys_start[], user_prog_nosys_end[];
extern const uint64_t user_prog_hlt_fault_off, user_prog_pf_fault_off;
/* Настоящий ELF userland/hello.c, встроенный в ядро (kernel/embed.S) */
extern const uint8_t hello_elf_start[], hello_elf_end[];

#define WATCHDOG_TICKS 100          /* 1 с: ни одна ring3-программа не может подвесить ядро дольше */
#define SPIN_TICKS      20          /* программа #3 должна быть отобрана по таймеру через ~200 мс */
#define EXEC_TICKS     500          /* 5 с на запуск настоящей программы */

static volatile int g_active;
static struct user_result *g_res;
static uint64_t g_t0, g_deadline;

static uint64_t g_pml4;
static struct iret_frame g_frame;      /* статический: должен быть виден и после смены CR3 */
static struct vmm_space g_us;          /* пространство текущего/последнего процесса */

/* ---- Страницы процесса. У pmm пока нет pmm_free, поэтому физические страницы берутся из
 * пула, который растёт один раз и переиспользуется между запусками (максимум POOL_PAGES*4 КиБ утечки). ---- */
#define POOL_PAGES 128
static uint64_t g_pool[POOL_PAGES];
static size_t g_pool_next;
static uint64_t g_mapped[POOL_PAGES];  /* VA всех страниц, отображённых в текущем сеансе — чтобы снять их у следующего */
static size_t g_nmapped;

static uint64_t alloc_page(void *ctx) { (void)ctx; return pmm_alloc(1); }
static void *p2v(uint64_t phys, void *ctx) { (void)ctx; return phys_to_virt(phys); }

/* Отображает новую ОБНУЛЁННУЮ страницу по va; возвращает указатель для записи из ядра или NULL */
static uint8_t *map_fresh(uint64_t va, uint64_t flags) {
    if (g_pool_next >= POOL_PAGES || g_nmapped >= POOL_PAGES) return NULL;
    if (!g_pool[g_pool_next]) { g_pool[g_pool_next] = pmm_alloc(1); if (!g_pool[g_pool_next]) return NULL; }
    uint64_t phys = g_pool[g_pool_next];
    uint8_t *mem = phys_to_virt(phys);
    memset(mem, 0, VMM_PAGE);
    /* без PTE_NX: бит NX работает только при EFER.NXE, а включён ли он — решает загрузчик */
    if (vmm_map(&g_us, va, phys, flags, alloc_page, p2v, NULL) != VMM_OK) return NULL;
    g_pool_next++;
    g_mapped[g_nmapped++] = va;
    return mem;
}

/* ---- Фаза 4: состояние системных вызовов процесса ---- */
static struct sys_state g_sys;
static char g_out[8192];
static size_t g_out_len;

static int op_copy_from_user(void *ctx, void *dst, uint64_t uva, size_t n) {
    (void)ctx;
    uint8_t *d = dst;
    while (n) {
        uint64_t ph, fl;
        if (vmm_translate(&g_us, uva & ~(VMM_PAGE - 1), &ph, &fl, p2v, NULL) != VMM_OK) return -1;
        if (!(fl & PTE_USER)) return -1;
        size_t off = (size_t)(uva & (VMM_PAGE - 1));
        size_t chunk = VMM_PAGE - off; if (chunk > n) chunk = n;
        memcpy(d, (const uint8_t *)phys_to_virt(ph) + off, chunk);
        d += chunk; uva += chunk; n -= chunk;
    }
    return 0;
}
static int op_copy_to_user(void *ctx, uint64_t uva, const void *src, size_t n) {
    (void)ctx;
    const uint8_t *s = src;
    while (n) {
        uint64_t ph, fl;
        if (vmm_translate(&g_us, uva & ~(VMM_PAGE - 1), &ph, &fl, p2v, NULL) != VMM_OK) return -1;
        if (!(fl & PTE_USER) || !(fl & PTE_WRITE)) return -1;     /* нельзя писать поверх .text программы через read() */
        size_t off = (size_t)(uva & (VMM_PAGE - 1));
        size_t chunk = VMM_PAGE - off; if (chunk > n) chunk = n;
        memcpy((uint8_t *)phys_to_virt(ph) + off, s, chunk);
        s += chunk; uva += chunk; n -= chunk;
    }
    return 0;
}
static int op_map_heap_page(void *ctx, uint64_t uva) {
    (void)ctx;
    if (uva < USER_HEAP_VA || uva >= USER_HEAP_VA + USER_HEAP_MAX || (uva & (VMM_PAGE - 1))) return -1;
    return map_fresh(uva, PTE_USER | PTE_WRITE) ? 0 : -1;
}
static void op_out(void *ctx, const char *buf, size_t n) {
    (void)ctx;
    for (size_t i = 0; i < n && g_out_len < sizeof g_out - 1; i++) g_out[g_out_len++] = buf[i];
    g_out[g_out_len] = 0;
}

/* Фаза 6: open() ищет файл в Memoria (vfs.h) от корня. Процесс пока не знает понятия "текущая
 * папка" — путь либо абсолютный ("/Desktop/foto.jpg"), либо ищется от корня. Данные принадлежат
 * VFS-узлу и не копируются: процесс только читает их, close() их не освобождает. */
static int op_vfs_open(void *ctx, const char *path, size_t pathlen, const uint8_t **out_data, size_t *out_len) {
    (void)ctx; (void)pathlen;
    int idx = vfs_resolve(VFS_ROOT, path);
    if (idx == VFS_NONE) return -1;
    if (vfs_is_dir(idx)) return -2;
    *out_data = vfs_data(idx);
    *out_len = vfs_len(idx);
    return 0;
}
/* Фаза 6: сокеты — тонкая обёртка над net.c. Таймауты здесь небольшие (ядро однопоточное,
 * "блокирующий" read() программы на деле ждёт максимум эти 2 с и тогда возвращает EAGAIN —
 * настоящей блокировки до появления планировщика нет). */
static int op_net_connect(void *ctx, uint32_t ip, uint16_t port) { (void)ctx; return tcp_connect(ip, port, 2000); }
static void op_net_close(void *ctx, int id) { (void)ctx; tcp_close(id); }
static long op_net_send(void *ctx, int id, const void *buf, size_t n) {
    (void)ctx;
    return tcp_send(id, buf, n) == 0 ? (long)n : -E_IO;
}
static long op_net_recv(void *ctx, int id, void *buf, size_t max) {
    (void)ctx;
    int n = tcp_recv(id, buf, max, 2000);
    if (n > 0) return n;
    if (n == 0) return -E_AGAIN;     /* таймаут: данных пока нет, соединение живо */
    if (n == -1) return 0;           /* собеседник закрыл: настоящий EOF */
    return -E_IO;                    /* -2: обрыв */
}

static const struct sys_ops g_ops = {
    NULL, op_copy_from_user, op_copy_to_user, op_map_heap_page, op_out,
    op_vfs_open, op_net_connect, op_net_close, op_net_send, op_net_recv
};

const char *user_output(void) { return g_out; }

static void snapshot(struct regs *r, int kind) {
    g_res->exit_kind = kind;
    g_res->int_no = r->int_no;
    g_res->err = r->err_code;
    g_res->rip = r->rip;  g_res->cs = r->cs;  g_res->ss = r->ss;
    g_res->rsp = r->rsp;  g_res->rflags = r->rflags;
    g_res->rax = r->rax;  g_res->rcx = r->rcx;
    g_res->cr2 = 0;
    if (r->int_no == 14) __asm__ volatile ("mov %%cr2, %0" : "=r"(g_res->cr2));
    g_res->ticks_in_user = timer_ticks - g_t0;
    g_res->exit_code = g_sys.exit_code;
    g_res->sys_calls = g_sys.calls;
    g_res->out_len = g_out_len;
}

void user_on_syscall(struct regs *r) {
    if (!g_active || !cs_is_user(r->cs)) return;
    int64_t ret = sys_dispatch(&g_sys, &g_ops, r->rax, r->rdi, r->rsi, r->rdx);
    if (g_sys.exited) {
        snapshot(r, USER_EXIT_SYSCALL);
        user_resume(USER_EXIT_SYSCALL);
    }
    r->rax = (uint64_t)ret;            /* isr_common вернёт это в rax пользователя через iretq */
}

void user_on_exception(struct regs *r) {
    if (!g_active || !cs_is_user(r->cs)) return;
    snapshot(r, USER_EXIT_FAULT);
    user_resume(USER_EXIT_FAULT);
}

void user_on_timer(struct regs *r) {
    if (!g_active || !cs_is_user(r->cs)) return;
    if (timer_ticks < g_deadline) return;
    snapshot(r, USER_EXIT_TIMEOUT);
    user_resume(USER_EXIT_TIMEOUT);
}

/* Подготовка нового сеанса: пространство процесса = свой PML4 + общая верхняя половина ядра,
 * все страницы прошлого сеанса сняты, состояние системных вызовов и вывод сброшены. */
static uint64_t g_kernel_cr3;
static int session_begin(struct user_result *out) {
    memset(out, 0, sizeof *out);
    if (g_active) { out->setup_err = -1; return -1; }
    g_kernel_cr3 = vmm_read_cr3();
    struct vmm_space kspace = { g_kernel_cr3 & PTE_ADDR_MASK };
    if (!g_pml4) { g_pml4 = pmm_alloc(1); if (!g_pml4) { out->setup_err = -3; return -3; } }
    g_us.pml4_phys = g_pml4;
    vmm_share_upper_half(&g_us, &kspace, p2v, NULL);
    for (size_t i = 0; i < g_nmapped; i++) vmm_unmap(&g_us, g_mapped[i], p2v, NULL);
    g_nmapped = 0; g_pool_next = 0;
    sys_state_init(&g_sys);
    g_out_len = 0; g_out[0] = 0;
    return 0;
}

/* Вход в ring3 и ожидание возврата (исключение / таймер / exit) */
static int session_run(struct user_result *out, uint64_t entry, uint64_t rsp, uint64_t limit) {
    if (iret_frame_build(&g_frame, entry, rsp) != 0) { out->setup_err = -5; return -5; }
    gdt_set_rsp0(gdt_ring0_stack_top());
    g_res = out;
    g_t0 = timer_ticks;
    g_deadline = g_t0 + limit;
    g_active = 1;

    uint64_t how = user_enter(&g_frame, g_pml4);   /* вернётся только через user_resume() */

    vmm_load_cr3(g_kernel_cr3);                    /* снова пространство ядра */
    g_active = 0;
    if (how != USER_EXIT_FAULT && how != USER_EXIT_TIMEOUT && how != USER_EXIT_SYSCALL) { out->setup_err = -6; return -6; }
    return 0;
}

int user_run(enum user_prog p, struct user_result *out) {
    const uint8_t *code; size_t len; uint64_t limit = WATCHDOG_TICKS;
    switch (p) {
    case USER_PROG_HLT:    code = user_prog_hlt_start;    len = (size_t)(user_prog_hlt_end - user_prog_hlt_start);       break;
    case USER_PROG_PF:     code = user_prog_pf_start;     len = (size_t)(user_prog_pf_end - user_prog_pf_start);         break;
    case USER_PROG_HELLO:  code = user_prog_hello_start;  len = (size_t)(user_prog_hello_end - user_prog_hello_start);   break;
    case USER_PROG_EFAULT: code = user_prog_efault_start; len = (size_t)(user_prog_efault_end - user_prog_efault_start); break;
    case USER_PROG_NOSYS:  code = user_prog_nosys_start;  len = (size_t)(user_prog_nosys_end - user_prog_nosys_start);   break;
    case USER_PROG_SPIN:   code = user_prog_spin_start;   len = (size_t)(user_prog_spin_end - user_prog_spin_start); limit = SPIN_TICKS; break;
    default: memset(out, 0, sizeof *out); out->setup_err = -2; return -2;
    }
    int rc = session_begin(out);
    if (rc) return rc;
    uint8_t *cp = map_fresh(USER_CODE_VA, PTE_USER);                        /* код: читать/исполнять, не писать */
    uint8_t *sp = map_fresh(USER_STACK_VA, PTE_USER | PTE_WRITE);
    if (!cp || !sp) { out->setup_err = -4; return -4; }
    memset(cp, 0xCC, VMM_PAGE);                                              /* 0xCC = int3: заблудившийся rip сразу виден */
    memcpy(cp, code, len);
    return session_run(out, USER_CODE_VA, USER_STACK_TOP, limit);
}

/* ---- Фаза 5: запуск настоящего ELF ---- */
static const char *g_setup_msg = "";
const char *user_setup_msg(void) { return g_setup_msg; }

static uint8_t *exec_page(void *ctx, uint64_t va, int writable) {
    (void)ctx;
    return map_fresh(va, PTE_USER | (writable ? PTE_WRITE : 0));
}

int user_exec(const uint8_t *file, size_t len, int argc, const char *const *argv, struct user_result *out) {
    memset(out, 0, sizeof *out);
    g_setup_msg = "";
    struct elf64_info info;
    enum elf_err ee = elf64_parse(file, len, &info);
    if (ee != ELF_OK) { g_setup_msg = elf64_err_str(ee); out->setup_err = -10; return -10; }
    int rc = session_begin(out);
    if (rc) { g_setup_msg = "не удалось подготовить адресное пространство"; return rc; }

    struct exec_ops ops = { NULL, exec_page };
    enum exec_err xe = exec_load(&info, file, len, &ops);
    if (xe != EXEC_OK) { g_setup_msg = exec_err_str(xe); out->setup_err = -11; return -11; }

    uint8_t *top_page = NULL;
    for (int i = 0; i < EXEC_STACK_PAGES; i++) {
        uint64_t va = USER_STACK_TOP - (uint64_t)(EXEC_STACK_PAGES - i) * VMM_PAGE;
        uint8_t *p = map_fresh(va, PTE_USER | PTE_WRITE);
        if (!p) { g_setup_msg = "не хватило памяти под стек"; out->setup_err = -12; return -12; }
        if (i == EXEC_STACK_PAGES - 1) top_page = p;
    }
    uint64_t rsp;
    xe = exec_build_stack(top_page, USER_STACK_TOP, argc, argv, &rsp);
    if (xe != EXEC_OK) { g_setup_msg = exec_err_str(xe); out->setup_err = -13; return -13; }
    return session_run(out, info.entry, rsp, EXEC_TICKS);
}

/* Показывает результат запуска пользователю. Вывод программы фильтруется: управляющие символы (кроме
 * \n, \t) заменяются, чтобы программа не могла перенастроить терминал escape-последовательностями. */
void user_report(struct term *t, int rc, const struct user_result *r) {
    if (rc != 0) {
        term_set_fg(t, TC_RED);
        term_printf(t, "Не удалось запустить: %s (код %d)\n", *g_setup_msg ? g_setup_msg : "ошибка подготовки", rc);
        term_set_fg(t, TC_DEFAULT);
        return;
    }
    const char *o = user_output();
    char line_buf[128]; size_t n = 0; int last_nl = 1;
    for (size_t i = 0; o[i]; i++) {
        unsigned char c = (unsigned char)o[i];
        if (c < 32 && c != '\n' && c != '\t') c = '?';
        line_buf[n++] = (char)c; last_nl = c == '\n';
        if (n == sizeof line_buf - 1) { line_buf[n] = 0; term_write(t, line_buf); n = 0; }
    }
    if (n) { line_buf[n] = 0; term_write(t, line_buf); }
    if (!last_nl) term_putc(t, '\n');
    if (r->out_len >= sizeof g_out - 1) { term_set_fg(t, TC_ORANGE); term_printf(t, "[вывод обрезан: больше %d байт]\n", (int)sizeof g_out - 1); term_set_fg(t, TC_DEFAULT); }

    if (r->exit_kind == USER_EXIT_SYSCALL) {
        term_set_fg(t, r->exit_code == 0 ? TC_GRAY : TC_ORANGE);
        term_printf(t, "[программа завершилась, код %d]\n", (int)r->exit_code);
    } else if (r->exit_kind == USER_EXIT_FAULT) {
        const char *nm = r->int_no == 14 ? "ошибка страницы #PF" : r->int_no == 13 ? "нарушение защиты #GP" :
                         r->int_no == 6 ? "неизвестная инструкция #UD" : r->int_no == 0 ? "деление на ноль #DE" : "исключение";
        term_set_fg(t, TC_RED);
        term_printf(t, "[программа аварийно остановлена: %s (вектор %d), RIP=0x%llx", nm, (int)r->int_no, (unsigned long long)r->rip);
        if (r->int_no == 14) term_printf(t, ", адрес=0x%llx, код=0x%llx", (unsigned long long)r->cr2, (unsigned long long)r->err);
        term_printf(t, "]\n");
    } else if (r->exit_kind == USER_EXIT_TIMEOUT) {
        term_set_fg(t, TC_RED);
        term_printf(t, "[программа прервана: работала дольше %d с]\n", EXEC_TICKS / 100);
    }
    term_set_fg(t, TC_DEFAULT);
}

/* Кладёт встроенный hello.elf в /System, чтобы его можно было запускать как обычный файл */
void user_register_builtin(void) {
    size_t n = (size_t)(hello_elf_end - hello_elf_start);
    uint8_t *copy = kmalloc(n);
    if (!copy) return;
    memcpy(copy, hello_elf_start, n);
    if (vfs_system == VFS_NONE || vfs_put(vfs_system, "hello.elf", copy, n) == VFS_NONE) kfree(copy);
}

/* lima run <файл> [аргументы]: ищет файл относительно cwd, затем в /System */
int user_run_file(struct term *t, int cwd, int argc, const char *const *argv) {
    int idx = vfs_resolve(cwd, argv[0]);
    if (idx == VFS_NONE && vfs_system != VFS_NONE) idx = vfs_resolve(vfs_system, argv[0]);
    if (idx == VFS_NONE || vfs_is_dir(idx)) {
        term_set_fg(t, TC_RED);
        term_printf(t, "Нет такого файла: %s\n", argv[0]);
        term_set_fg(t, TC_DEFAULT);
        return -1;
    }
    struct user_result r;
    int rc = user_exec(vfs_data(idx), vfs_len(idx), argc, argv, &r);
    user_report(t, rc, &r);
    return rc;
}

static void line(struct term *t, int ok, const char *name, const char *detail) {
    term_set_fg(t, ok ? TC_GREEN : TC_RED);
    term_printf(t, "  [%s] ", ok ? "OK" : "ПРОВАЛ");
    term_set_fg(t, TC_DEFAULT);
    term_printf(t, "%s", name);
    term_set_fg(t, TC_GRAY);
    term_printf(t, " — %s\n", detail);
    term_set_fg(t, TC_DEFAULT);
}

int user_selftest(struct term *t) {
    struct user_result r;
    int fails = 0, ok;

    term_set_fg(t, TC_ACCENT);
    term_printf(t, "Фаза 3: ring3 (пользовательский режим)\n");
    term_set_fg(t, TC_DEFAULT);

    int rc = user_run(USER_PROG_HLT, &r);
    ok = rc == 0 && user_check_hlt(&r, user_prog_hlt_fault_off);
    line(t, ok, "вход в ring3 + #GP на hlt", ok ? "CS=0x23 SS=0x1B, стек пользователя работает" : "не совпало с ожидаемым");
    if (!ok) {
        fails++;
        term_printf(t, "      rc=%d vec=%llu err=0x%llx cs=0x%llx rip=0x%llx rax=0x%llx rsp=0x%llx\n", rc,
                    (unsigned long long)r.int_no, (unsigned long long)r.err, (unsigned long long)r.cs,
                    (unsigned long long)r.rip, (unsigned long long)r.rax, (unsigned long long)r.rsp);
    }

    rc = user_run(USER_PROG_PF, &r);
    ok = rc == 0 && user_check_pf(&r, user_prog_pf_fault_off);
    line(t, ok, "защита памяти ядра от ring3", ok ? "#PF, код 0x5 (страница есть, прав нет), CR2=начало ядра" : "не совпало с ожидаемым");
    if (!ok) {
        fails++;
        term_printf(t, "      rc=%d vec=%llu err=0x%llx cr2=0x%llx rip=0x%llx\n", rc,
                    (unsigned long long)r.int_no, (unsigned long long)r.err,
                    (unsigned long long)r.cr2, (unsigned long long)r.rip);
    }

    rc = user_run(USER_PROG_SPIN, &r);
    ok = rc == 0 && user_check_spin(&r, SPIN_TICKS);
    line(t, ok, "таймер отбирает управление у бесконечного цикла", ok ? "IRQ0 из ring3 -> стек TSS.rsp0 -> возврат в ядро" : "не совпало с ожидаемым");
    if (!ok) {
        fails++;
        term_printf(t, "      rc=%d exit=%d cs=0x%llx rip=0x%llx ticks=%llu\n", rc, r.exit_kind,
                    (unsigned long long)r.cs, (unsigned long long)r.rip, (unsigned long long)r.ticks_in_user);
    }

    term_set_fg(t, TC_ACCENT);
    term_printf(t, "Фаза 4: системные вызовы (int 0x80, номера как в Linux)\n");
    term_set_fg(t, TC_DEFAULT);

    rc = user_run(USER_PROG_HELLO, &r);
    ok = rc == 0 && user_check_hello(&r, user_output());
    line(t, ok, "write + brk + exit из ring3", ok ? "программа сама вывела строку, выросла куча (2 страницы), exit(42)" : "не совпало с ожидаемым");
    if (ok) { term_set_fg(t, TC_GRAY); term_printf(t, "      вывод программы: %s", user_output()); term_set_fg(t, TC_DEFAULT); }
    else {
        fails++;
        term_printf(t, "      rc=%d exit=%d code=%lld calls=%u out=%llu vec=%llu rip=0x%llx\n", rc, r.exit_kind,
                    (long long)r.exit_code, r.sys_calls, (unsigned long long)r.out_len,
                    (unsigned long long)r.int_no, (unsigned long long)r.rip);
    }

    rc = user_run(USER_PROG_EFAULT, &r);
    ok = rc == 0 && user_check_errno(&r, E_FAULT);
    line(t, ok, "write с указателем на память ядра", ok ? "ядро ответило -EFAULT, в ядро ничего не утекло" : "не совпало с ожидаемым");
    if (!ok) { fails++; term_printf(t, "      rc=%d exit=%d code=%lld calls=%u out=%llu\n", rc, r.exit_kind, (long long)r.exit_code, r.sys_calls, (unsigned long long)r.out_len); }

    rc = user_run(USER_PROG_NOSYS, &r);
    ok = rc == 0 && user_check_errno(&r, E_NOSYS);
    line(t, ok, "неизвестный системный вызов", ok ? "ядро ответило -ENOSYS" : "не совпало с ожидаемым");
    if (!ok) { fails++; term_printf(t, "      rc=%d exit=%d code=%lld calls=%u out=%llu\n", rc, r.exit_kind, (long long)r.exit_code, r.sys_calls, (unsigned long long)r.out_len); }

    term_set_fg(t, TC_ACCENT);
    term_printf(t, "Фаза 5: настоящий ELF-файл + мини-libc\n");
    term_set_fg(t, TC_DEFAULT);

    {
        const char *av[] = { "hello.elf", "LimonOS" };
        rc = user_exec(hello_elf_start, (size_t)(hello_elf_end - hello_elf_start), 2, av, &r);
        ok = rc == 0 && r.exit_kind == USER_EXIT_SYSCALL && r.exit_code == 0 && strcmp(user_output(), EXEC_HELLO_EXPECT) == 0;
        line(t, ok, "hello.elf (gcc+ld, своя libc): загрузка и запуск", ok ? "вывод программы совпал с ожидаемым побайтно, код 0" : "не совпало с ожидаемым");
        if (!ok) {
            fails++;
            term_printf(t, "      rc=%d (%s) exit=%d code=%lld vec=%llu rip=0x%llx cr2=0x%llx calls=%u\n      вывод: [%s]\n", rc, user_setup_msg(), r.exit_kind,
                        (long long)r.exit_code, (unsigned long long)r.int_no, (unsigned long long)r.rip,
                        (unsigned long long)r.cr2, r.sys_calls, user_output());
        }
    }

    term_set_fg(t, fails ? TC_RED : TC_GREEN);
    term_printf(t, fails ? "Провалено проверок: %d\n" : "Все 7 проверок прошли. Ядро живо.\n", fails);
    term_set_fg(t, TC_DEFAULT);
    return fails;
}
