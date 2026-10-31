#include "sched.h"
#include "mem.h"
#include "vmm.h"
#include "timer.h"
#include "kstring.h"
#include "format.h"
#include "syscall.h"
#include "elf.h"
#include "exec.h"
#include "vfs.h"
#include "ring3.h"
#include "net.h"
#include "user.h"     /* user_enter/user_resume — тот же вход в ring3, что и у Фазы 3 */

#define QUANTUM_TICKS 5         /* 5 тиков по 10 мс = ~50 мс на процесс за раз — чередование заметно глазом */
#define TOTAL_TICKS   1000      /* 10 с суммарного времени каждому процессу — страховка от зависшего цикла */
#define POOL_PAGES    48        /* страниц на процесс; 4 процесса * 48 = 192 страницы = 768 КиБ с запасом */

struct proc {
    enum proc_state state;
    uint64_t pml4;
    struct vmm_space space;
    struct sys_state sys;
    struct regs saved;          /* полный снимок регистров+CS/SS/RSP/RFLAGS, когда не выполняется прямо сейчас */
    char out[2048]; size_t out_len;
    char name[24];
    unsigned quanta;
    uint64_t quantum_left;      /* тиков до конца текущего кванта */
    uint64_t ticks_total;
    int exit_code;
    const char *fault_desc;

    uint64_t pool[POOL_PAGES]; size_t pool_next;
    uint64_t mapped[POOL_PAGES]; size_t nmapped;
};

static struct proc g_proc[SCHED_MAX_PROC];
static int g_cur = -1;                 /* слот, который выполняется прямо сейчас, или -1 */
static int g_active;                   /* идёт ли сейчас sched_run_queue() */
static uint64_t g_kernel_cr3;
static char g_trace[300]; static size_t g_trace_len;
static const char *g_last_err = "";

static uint64_t alloc_page_cb(void *c) { (void)c; return pmm_alloc(1); }
static void *p2v_cb(uint64_t phys, void *c) { (void)c; return phys_to_virt(phys); }

static uint8_t *proc_map_fresh(struct proc *p, uint64_t va, uint64_t flags) {
    if (p->pool_next >= POOL_PAGES || p->nmapped >= POOL_PAGES) return NULL;
    if (!p->pool[p->pool_next]) { p->pool[p->pool_next] = pmm_alloc(1); if (!p->pool[p->pool_next]) return NULL; }
    uint64_t phys = p->pool[p->pool_next];
    uint8_t *mem = phys_to_virt(phys);
    memset(mem, 0, VMM_PAGE);
    if (vmm_map(&p->space, va, phys, flags, alloc_page_cb, p2v_cb, NULL) != VMM_OK) return NULL;
    p->pool_next++;
    p->mapped[p->nmapped++] = va;
    return mem;
}

/* ---- sys_ops: ctx = struct proc* этого слота ---- */
static int op_copy_from_user(void *ctx, void *dst, uint64_t uva, size_t n) {
    struct proc *p = ctx; uint8_t *d = dst;
    while (n) {
        uint64_t ph, fl;
        if (vmm_translate(&p->space, uva & ~(VMM_PAGE - 1), &ph, &fl, p2v_cb, NULL) != VMM_OK) return -1;
        if (!(fl & PTE_USER)) return -1;
        size_t off = (size_t)(uva & (VMM_PAGE - 1));
        size_t chunk = VMM_PAGE - off; if (chunk > n) chunk = n;
        memcpy(d, (const uint8_t *)phys_to_virt(ph) + off, chunk);
        d += chunk; uva += chunk; n -= chunk;
    }
    return 0;
}
static int op_copy_to_user(void *ctx, uint64_t uva, const void *src, size_t n) {
    struct proc *p = ctx; const uint8_t *s = src;
    while (n) {
        uint64_t ph, fl;
        if (vmm_translate(&p->space, uva & ~(VMM_PAGE - 1), &ph, &fl, p2v_cb, NULL) != VMM_OK) return -1;
        if (!(fl & PTE_USER) || !(fl & PTE_WRITE)) return -1;
        size_t off = (size_t)(uva & (VMM_PAGE - 1));
        size_t chunk = VMM_PAGE - off; if (chunk > n) chunk = n;
        memcpy((uint8_t *)phys_to_virt(ph) + off, s, chunk);
        s += chunk; uva += chunk; n -= chunk;
    }
    return 0;
}
static int op_map_heap_page(void *ctx, uint64_t uva) {
    struct proc *p = ctx;
    if (uva < USER_HEAP_VA || uva >= USER_HEAP_VA + USER_HEAP_MAX || (uva & (VMM_PAGE - 1))) return -1;
    return proc_map_fresh(p, uva, PTE_USER | PTE_WRITE) ? 0 : -1;
}
static void op_out(void *ctx, const char *buf, size_t n) {
    struct proc *p = ctx;
    for (size_t i = 0; i < n && p->out_len < sizeof p->out - 1; i++) p->out[p->out_len++] = buf[i];
    p->out[p->out_len] = 0;
}
static int op_vfs_open(void *ctx, const char *path, size_t pathlen, const uint8_t **data, size_t *len) {
    (void)ctx; (void)pathlen;
    int idx = vfs_resolve(VFS_ROOT, path);
    if (idx == VFS_NONE) return -1;
    if (vfs_is_dir(idx)) return -2;
    *data = vfs_data(idx); *len = vfs_len(idx);
    return 0;
}
static int  op_net_connect(void *ctx, uint32_t ip, uint16_t port) { (void)ctx; return tcp_connect(ip, port, 2000); }
static void op_net_close(void *ctx, int id) { (void)ctx; tcp_close(id); }
static long op_net_send(void *ctx, int id, const void *buf, size_t n) { (void)ctx; return tcp_send(id, buf, n) == 0 ? (long)n : -E_IO; }
static long op_net_recv(void *ctx, int id, void *buf, size_t max) {
    (void)ctx;
    int n = tcp_recv(id, buf, max, 2000);
    if (n > 0) return n;
    if (n == 0) return -E_AGAIN;
    if (n == -1) return 0;
    return -E_IO;
}
static const struct sys_ops g_sops_template = {
    NULL, op_copy_from_user, op_copy_to_user, op_map_heap_page, op_out,
    op_vfs_open, op_net_connect, op_net_close, op_net_send, op_net_recv
};

static uint8_t *exec_page_cb(void *ctx, uint64_t va, int writable) {
    return proc_map_fresh((struct proc *)ctx, va, PTE_USER | (writable ? PTE_WRITE : 0));
}

int sched_spawn(const uint8_t *file, size_t len, int argc, const char *const *argv) {
    int slot = -1;
    for (int i = 0; i < SCHED_MAX_PROC; i++) if (g_proc[i].state == PROC_FREE) { slot = i; break; }
    if (slot < 0) { g_last_err = "нет свободных слотов (максимум 4 процесса сразу, сначала go/дождись завершения)"; return -1; }
    struct proc *p = &g_proc[slot];
    memset(p, 0, sizeof *p);

    struct elf64_info info;
    enum elf_err ee = elf64_parse(file, len, &info);
    if (ee != ELF_OK) { g_last_err = elf64_err_str(ee); return -2; }

    uint64_t kcr3 = vmm_read_cr3();
    struct vmm_space kspace = { kcr3 & PTE_ADDR_MASK };
    p->pml4 = pmm_alloc(1);
    if (!p->pml4) { g_last_err = "не хватило памяти под таблицы страниц"; return -3; }
    p->space.pml4_phys = p->pml4;
    vmm_share_upper_half(&p->space, &kspace, p2v_cb, NULL);

    struct exec_ops ops = { p, exec_page_cb };
    enum exec_err xe = exec_load(&info, file, len, &ops);
    if (xe != EXEC_OK) { g_last_err = exec_err_str(xe); return -4; }

    uint8_t *top_page = NULL;
    for (int i = 0; i < EXEC_STACK_PAGES; i++) {
        uint64_t va = USER_STACK_TOP - (uint64_t)(EXEC_STACK_PAGES - i) * VMM_PAGE;
        uint8_t *pg = proc_map_fresh(p, va, PTE_USER | PTE_WRITE);
        if (!pg) { g_last_err = "не хватило памяти под стек"; return -5; }
        if (i == EXEC_STACK_PAGES - 1) top_page = pg;
    }
    uint64_t rsp;
    xe = exec_build_stack(top_page, USER_STACK_TOP, argc, argv, &rsp);
    if (xe != EXEC_OK) { g_last_err = exec_err_str(xe); return -6; }

    sys_state_init(&p->sys);
    p->saved.rip = info.entry;
    p->saved.cs = SEL_UCODE;
    p->saved.ss = SEL_UDATA;
    p->saved.rsp = rsp;
    p->saved.rflags = RFLAGS_RESERVED1 | RFLAGS_IF;
    p->quantum_left = QUANTUM_TICKS;
    p->state = PROC_RUNNABLE;
    size_t i = 0;
    for (; argc > 0 && argv[0][i] && i < sizeof p->name - 1; i++) p->name[i] = argv[0][i];
    p->name[i] = 0;
    return slot;
}

const char *sched_spawn_err(int code) { (void)code; return g_last_err; }

int sched_list(struct proc_info *out, int max) {
    int n = 0;
    for (int i = 0; i < SCHED_MAX_PROC && n < max; i++) {
        if (g_proc[i].state == PROC_FREE) continue;
        out[n].slot = i;
        out[n].state = g_proc[i].state;
        memcpy(out[n].name, g_proc[i].name, sizeof out[n].name);
        out[n].quanta = g_proc[i].quanta;
        out[n].exit_code = g_proc[i].exit_code;
        out[n].fault_desc = g_proc[i].fault_desc;
        n++;
    }
    return n;
}

int sched_active(void) { return g_active; }

static void trace_add(int slot) {
    if (g_trace_len + 3 >= sizeof g_trace) return;
    int n = ksnprintf(g_trace + g_trace_len, sizeof g_trace - g_trace_len, "%d ", slot);
    if (n > 0) g_trace_len += (size_t)n;
}

/* Переключение: ищет следующий PROC_RUNNABLE по кругу от g_cur. Если находит — кладёт его
 * полный снимок регистров в *r (isr_common потом именно это вернёт в ring3 через iretq) и
 * меняет CR3 ПРЯМО СЕЙЧАС (ядро всё ещё выполняется на общем верхнем стеке, он отображён
 * одинаково во всех процессах — переключение CR3 этот стек не портит). Если очередь пуста —
 * восстанавливает CR3 ядра и возвращается в sched_run_queue() через user_resume(). */
static void schedule_next(struct regs *r) {
    int start = g_cur < 0 ? 0 : g_cur;
    for (int tries = 1; tries <= SCHED_MAX_PROC; tries++) {
        int i = (start + tries) % SCHED_MAX_PROC;
        if (g_proc[i].state == PROC_RUNNABLE) {
            g_cur = i;
            *r = g_proc[i].saved;
            g_proc[i].quantum_left = QUANTUM_TICKS;
            g_proc[i].quanta++;
            trace_add(i);
            vmm_load_cr3(g_proc[i].pml4);
            return;
        }
    }
    g_cur = -1;
    vmm_load_cr3(g_kernel_cr3);
    user_resume(0);
}

void sched_on_syscall(struct regs *r) {
    if (g_cur < 0) return;
    struct proc *p = &g_proc[g_cur];
    struct sys_ops ops = g_sops_template; ops.ctx = p;
    int64_t ret = sys_dispatch(&p->sys, &ops, r->rax, r->rdi, r->rsi, r->rdx);
    if (p->sys.exited) {
        p->state = PROC_DONE;
        p->exit_code = (int)p->sys.exit_code;
        schedule_next(r);
        return;
    }
    r->rax = (uint64_t)ret;
}

static const char *fault_name(uint64_t vec) {
    switch (vec) {
    case 0:  return "деление на ноль (#DE)";
    case 6:  return "неверная инструкция (#UD)";
    case 13: return "нарушение защиты (#GP)";
    case 14: return "ошибка страницы (#PF)";
    default: return "исключение процессора";
    }
}
void sched_on_exception(struct regs *r) {
    if (g_cur < 0) return;
    g_proc[g_cur].state = PROC_FAULTED;
    g_proc[g_cur].fault_desc = fault_name(r->int_no);
    schedule_next(r);
}

void sched_on_timer(struct regs *r) {
    if (g_cur < 0) return;
    struct proc *p = &g_proc[g_cur];
    p->ticks_total++;
    if (p->ticks_total >= TOTAL_TICKS) { p->state = PROC_TIMEOUT; schedule_next(r); return; }
    if (p->quantum_left > 1) { p->quantum_left--; return; }   /* квант ещё не кончился: не трогаем r, isr_common вернёт управление тому же процессу */
    p->saved = *r;                                             /* квант кончился: сохранить точное место, где остановили */
    schedule_next(r);
}

static void report_one(struct term *t, struct proc *p, int slot) {
    term_set_fg(t, TC_ACCENT); term_printf(t, "[%d] %s — ", slot, p->name[0] ? p->name : "?"); term_set_fg(t, TC_DEFAULT);
    if (p->state == PROC_DONE) {
        term_set_fg(t, p->exit_code ? TC_ORANGE : TC_GRAY);
        term_printf(t, "завершился, код %d, квантов получил %u\n", p->exit_code, p->quanta);
    } else if (p->state == PROC_FAULTED) {
        term_set_fg(t, TC_RED);
        term_printf(t, "аварийно остановлен: %s, RIP=0x%llx, квантов получил %u\n", p->fault_desc, (unsigned long long)p->saved.rip, p->quanta);
    } else if (p->state == PROC_TIMEOUT) {
        term_set_fg(t, TC_RED);
        term_printf(t, "снят по общему лимиту времени (%d с), квантов получил %u\n", TOTAL_TICKS / 100, p->quanta);
    }
    term_set_fg(t, TC_DEFAULT);
    if (p->out_len) {
        term_set_fg(t, TC_GRAY);
        term_write(t, p->out);
        term_set_fg(t, TC_DEFAULT);
        if (p->out[p->out_len - 1] != '\n') term_putc(t, '\n');
    }
}

void sched_run_queue(struct term *t) {
    if (g_active) { term_printf(t, "Диспетчер уже выполняется.\n"); return; }
    int first = -1;
    for (int i = 0; i < SCHED_MAX_PROC; i++) if (g_proc[i].state == PROC_RUNNABLE) { first = i; break; }
    if (first < 0) { term_printf(t, "Очередь пуста: сначала «bg <файл.elf>».\n"); return; }

    g_kernel_cr3 = vmm_read_cr3();
    g_trace_len = 0;
    g_active = 1;
    g_cur = first;
    g_proc[first].quantum_left = QUANTUM_TICKS;
    g_proc[first].quanta = 1;
    trace_add(first);

    struct iret_frame frame;
    if (iret_frame_build(&frame, g_proc[first].saved.rip, g_proc[first].saved.rsp) != 0) {
        g_active = 0; g_cur = -1;
        term_set_fg(t, TC_RED); term_printf(t, "Не удалось подготовить запуск (плохой адрес входа/стека)\n"); term_set_fg(t, TC_DEFAULT);
        return;
    }
    gdt_set_rsp0(gdt_ring0_stack_top());
    user_enter(&frame, g_proc[first].pml4);   /* вернётся сюда только когда schedule_next() не найдёт больше ни одного PROC_RUNNABLE */
    vmm_load_cr3(g_kernel_cr3);
    g_active = 0;

    term_set_fg(t, TC_ACCENT);
    term_printf(t, "Чередование квантов (по ~%d мс, какой слот выполнялся): %s\n", QUANTUM_TICKS * 10, g_trace);
    term_set_fg(t, TC_DEFAULT);
    for (int i = 0; i < SCHED_MAX_PROC; i++) {
        if (g_proc[i].state == PROC_FREE) continue;
        report_one(t, &g_proc[i], i);
        g_proc[i].state = PROC_FREE;         /* "пожали" процесс (как wait() в Unix) — слот свободен для следующих */
    }
}
