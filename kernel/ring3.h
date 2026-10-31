#ifndef LIMON_RING3_H
#define LIMON_RING3_H
#include <stdint.h>
#include <stddef.h>

/*
 * Фаза 3 плана (docs/USERSPACE_PLAN.md): GDT с пользовательскими сегментами,
 * TSS и переход в ring3.
 *
 * Здесь только "чистая" часть: никаких asm-инструкций, никакого доступа к
 * железу — построение дескрипторов, TSS, кадра для iretq и разбор кода
 * ошибки page fault. Поэтому она проверяется на хосте (tests/gdt_host.c).
 * Всё, что трогает реальный процессор (lgdt/ltr/iretq/cr3), лежит в gdt.c,
 * user.c и usermode.S и на хосте проверено быть не может.
 */

/* ---- Селекторы. Порядок выбран так, чтобы в Фазе 4 можно было включить
 * syscall/sysret без перестройки GDT: SYSRET берёт SS = STAR[63:48]+8 и
 * CS = STAR[63:48]+16, то есть "данные пользователя" обязаны лежать ровно
 * на 8 байт ниже "кода пользователя" (Intel SDM vol.2, SYSRET). ---- */
#define SEL_NULL        0x00
#define SEL_KCODE       0x08
#define SEL_KDATA       0x10
#define SEL_UDATA_BASE  0x18          /* индекс в GDT, без RPL */
#define SEL_UCODE_BASE  0x20
#define SEL_TSS         0x28          /* занимает ДВЕ записи GDT (16 байт) */
#define SEL_UDATA       (SEL_UDATA_BASE | 3)   /* 0x1B */
#define SEL_UCODE       (SEL_UCODE_BASE | 3)   /* 0x23 */

#define GDT_ENTRIES     7             /* null, kcode, kdata, udata, ucode, tss(2) */

/* Типы доступа (байт access): P=1, S, DPL, тип */
#define ACC_KCODE   0x9A
#define ACC_KDATA   0x92
#define ACC_UDATA   0xF2
#define ACC_UCODE   0xFA
#define ACC_TSS     0x89              /* P=1, DPL=0, тип 9: свободный 64-битный TSS */

/* Старший ниббл флагов: G=1; для кода ещё L=1 (64 бита) и D=0; для данных D/B=1 */
#define FLG_CODE64  0xA
#define FLG_DATA    0xC

static inline uint64_t gdt_make_desc(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    uint64_t d = 0;
    d |= (uint64_t)(limit & 0xFFFF);
    d |= (uint64_t)(base & 0xFFFFFF) << 16;
    d |= (uint64_t)access << 40;
    d |= (uint64_t)((limit >> 16) & 0xF) << 48;
    d |= (uint64_t)(flags & 0xF) << 52;
    d |= (uint64_t)((base >> 24) & 0xFF) << 56;
    return d;
}

/* 64-битный системный дескриптор (TSS): 16 байт = две записи GDT */
static inline void gdt_make_tss_desc(uint64_t base, uint32_t limit, uint64_t out[2]) {
    out[0] = gdt_make_desc((uint32_t)base, limit, ACC_TSS, 0x0);
    out[1] = base >> 32;              /* старшие 32 бита базы, остальное зарезервировано = 0 */
}

/* Заполняет всю таблицу GDT. tss_base — виртуальный адрес структуры TSS. */
static inline void gdt_fill(uint64_t gdt[GDT_ENTRIES], uint64_t tss_base, uint32_t tss_limit) {
    gdt[0] = 0;
    gdt[1] = gdt_make_desc(0, 0xFFFFF, ACC_KCODE, FLG_CODE64);
    gdt[2] = gdt_make_desc(0, 0xFFFFF, ACC_KDATA, FLG_DATA);
    gdt[3] = gdt_make_desc(0, 0xFFFFF, ACC_UDATA, FLG_DATA);
    gdt[4] = gdt_make_desc(0, 0xFFFFF, ACC_UCODE, FLG_CODE64);
    gdt_make_tss_desc(tss_base, tss_limit, &gdt[5]);
}

/* ---- 64-битный TSS (Intel SDM vol.3 §7.7): 104 байта ---- */
struct __attribute__((packed)) tss64 {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;        /* стеки для переходов в ring0/1/2: нужен только rsp0 */
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;              /* >= размера TSS => карты портов нет, ring3 в порты не лезет */
};
#define TSS_SIZE 104

/* ---- Кадр для iretq: порядок как его снимает процессор (с низшего адреса) ---- */
#define RFLAGS_RESERVED1 0x2ULL       /* бит 1 всегда 1 */
#define RFLAGS_IF        0x200ULL
#define RFLAGS_IOPL_MASK 0x3000ULL    /* IOPL=0: ring3 не может cli/sti/in/out */

struct iret_frame { uint64_t rip, cs, rflags, rsp, ss; };

/* Верхняя граница пользовательской половины (каноническая, bit47=0) */
#define USER_VA_END 0x0000800000000000ULL

static inline int user_addr_ok(uint64_t a) { return a < USER_VA_END; }

/* 0 — успех; -1 — адрес не пользовательский/не каноничный (iretq с таким rip/rsp
 * дал бы #GP уже В ЯДРЕ, поэтому проверяем заранее). */
static inline int iret_frame_build(struct iret_frame *f, uint64_t rip, uint64_t rsp) {
    if (!user_addr_ok(rip) || !user_addr_ok(rsp)) return -1;
    f->rip = rip;
    f->cs = SEL_UCODE;
    f->rflags = RFLAGS_RESERVED1 | RFLAGS_IF;     /* прерывания включены, IOPL=0 */
    f->rsp = rsp;
    f->ss = SEL_UDATA;
    return 0;
}

/* ---- Разбор кода ошибки #PF (Intel SDM vol.3 §4.7) ---- */
#define PFE_P    0x01      /* 1 = страница была, нарушение прав; 0 = страницы нет */
#define PFE_W   0x02      /* 1 = запись */
#define PFE_U    0x04      /* 1 = обращение из ring3 */
#define PFE_RSVD 0x08      /* 1 = зарезервированный бит в таблице страниц */
#define PFE_I    0x10      /* 1 = выборка инструкции */

static inline int cs_is_user(uint64_t cs) { return (cs & 3) == 3; }

/* ---- Раскладка пользовательского адресного пространства демо-программ ---- */
#define USER_CODE_VA    0x0000000000400000ULL
#define USER_STACK_VA   0x00007FFFFFFFE000ULL           /* одна страница стека */
#define USER_STACK_TOP  (USER_STACK_VA + 0x1000ULL)
#define USER_MAGIC      0x4C494D4F4E4F5333ULL            /* 'LIMONOS3' — кладёт в rax программа #1 */
#define KERNEL_TEXT_VA  0xFFFFFFFF80000000ULL            /* что читает программа #2 */

enum user_exit { USER_EXIT_NONE = 0, USER_EXIT_FAULT = 1, USER_EXIT_TIMEOUT = 2, USER_EXIT_SYSCALL = 3 /* программа сама вызвала exit */ };

/* Снимок состояния ring3-программы в момент, когда ядро забрало управление */
struct user_result {
    int      setup_err;      /* 0 или отрицательный код, если не удалось подготовить запуск */
    int      exit_kind;      /* enum user_exit */
    uint64_t int_no, err, cr2;
    uint64_t rip, cs, ss, rsp, rflags, rax, rcx;
    uint64_t ticks_in_user;  /* сколько тиков таймера (100 Гц) программа выполнялась */
    /* Фаза 4 */
    int64_t  exit_code;      /* код, с которым программа вызвала exit() */
    unsigned sys_calls;      /* сколько системных вызовов она сделала */
    size_t   out_len;        /* сколько байт она вывела через write(1|2) */
};

/* Проверки результата. Чистые функции — на хосте проверяется, что они принимают
 * ровно то, что должно произойти по спецификации, и отвергают всё остальное.
 * Что процессор РЕАЛЬНО выдаст именно это — покажет только запуск на QEMU/железе. */
static inline int user_check_hlt(const struct user_result *r, uint64_t fault_off) {
    return r->setup_err == 0 && r->exit_kind == USER_EXIT_FAULT
        && r->int_no == 13 && r->err == 0                    /* #GP(0): hlt в ring3 */
        && cs_is_user(r->cs) && r->cs == SEL_UCODE && r->ss == SEL_UDATA
        && r->rip == USER_CODE_VA + fault_off
        && r->rax == USER_MAGIC && r->rcx == USER_MAGIC      /* стек пользователя: push/pop сработали */
        && r->rsp == USER_STACK_TOP
        && (r->rflags & RFLAGS_IF) && !(r->rflags & RFLAGS_IOPL_MASK);
}
static inline int user_check_pf(const struct user_result *r, uint64_t fault_off) {
    return r->setup_err == 0 && r->exit_kind == USER_EXIT_FAULT
        && r->int_no == 14
        && r->err == (PFE_P | PFE_U)                            /* страница есть, права нарушены, из ring3, чтение */
        && r->cr2 == KERNEL_TEXT_VA
        && r->cs == SEL_UCODE && r->ss == SEL_UDATA
        && r->rip == USER_CODE_VA + fault_off;
}
static inline int user_check_spin(const struct user_result *r, uint64_t min_ticks) {
    return r->setup_err == 0 && r->exit_kind == USER_EXIT_TIMEOUT
        && r->cs == SEL_UCODE && r->ss == SEL_UDATA
        && r->rip == USER_CODE_VA                             /* jmp $ — крутится на одном месте */
        && r->ticks_in_user >= min_ticks;
}


/* ---- Фаза 4: проверки программ, которые общаются с ядром через int 0x80 ---- */
#define USER_HELLO_MSG "Привет из ring3!\n"
#define USER_HELLO_EXIT 42

static inline int user_bytes_eq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
/* hello: write + brk(0) + brk(+8 КиБ, запись/чтение кучи) + exit(42) = 4 вызова */
static inline int user_check_hello(const struct user_result *r, const char *out) {
    const char *m = USER_HELLO_MSG;
    size_t ml = sizeof(USER_HELLO_MSG) - 1;
    return r->setup_err == 0 && r->exit_kind == USER_EXIT_SYSCALL
        && r->exit_code == USER_HELLO_EXIT && r->sys_calls == 4
        && r->cs == SEL_UCODE && r->ss == SEL_UDATA
        && r->out_len == ml && user_bytes_eq(out, m, ml);
}
/* Программа сделала один вызов, получила -errno и вышла с кодом errno (2 вызова, вывода нет) */
static inline int user_check_errno(const struct user_result *r, int64_t errno_expected) {
    return r->setup_err == 0 && r->exit_kind == USER_EXIT_SYSCALL
        && r->exit_code == errno_expected && r->sys_calls == 2 && r->out_len == 0
        && r->cs == SEL_UCODE && r->ss == SEL_UDATA;
}

#endif
