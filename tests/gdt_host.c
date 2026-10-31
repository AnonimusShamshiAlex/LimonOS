/* Проверка чистой части Фазы 3 (kernel/ring3.h + склейка с kernel/vmm.c):
 * дескрипторы GDT/TSS, кадр iretq, разбор #PF, проверки результата и раскладка
 * пользовательского адресного пространства.
 *
 * Дескрипторы проверяются ДВУМЯ независимыми путями: (а) сравнением с хорошо
 * известными каноническими значениями (0x00AFFA000000FFFF и т.п. — их знает любой
 * учебник по x86_64) и (б) отдельным декодером битовых полей, написанным здесь,
 * а не копией кода сборки дескриптора.
 *
 * НЕ проверяется и не может быть проверено на хосте: что процессор примет GDT
 * (lgdt), TSS (ltr), выполнит iretq в ring3 и вернётся в ядро. Это — QEMU/железо. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "arch.h"
#include "ring3.h"
#include "vmm.h"

static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %d: %s\n", __LINE__, #c); fails++; } }while(0)

/* --- независимый декодер дескриптора сегмента (Intel SDM vol.3 §3.4.5) --- */
struct dec { uint32_t base, limit; int type, s, dpl, p, avl, l, db, g; };
static struct dec decode(uint64_t d) {
    struct dec x;
    x.limit = (uint32_t)((d & 0xFFFF) | (((d >> 48) & 0xF) << 16));
    x.base  = (uint32_t)(((d >> 16) & 0xFFFFFF) | (((d >> 56) & 0xFF) << 24));
    x.type  = (int)((d >> 40) & 0xF);
    x.s     = (int)((d >> 44) & 1);
    x.dpl   = (int)((d >> 45) & 3);
    x.p     = (int)((d >> 47) & 1);
    x.avl   = (int)((d >> 52) & 1);
    x.l     = (int)((d >> 53) & 1);
    x.db    = (int)((d >> 54) & 1);
    x.g     = (int)((d >> 55) & 1);
    return x;
}

/* --- поддельная физическая память для проверки склейки с vmm.c --- */
#define ARENA_PAGES 64
struct fake_phys { uint8_t *arena; uint64_t base, next; };
static uint64_t fake_alloc(void *c) { struct fake_phys *f = c; uint64_t p = f->next; f->next += VMM_PAGE; return p; }
static void *fake_p2v(uint64_t p, void *c) { struct fake_phys *f = c; return f->arena + (p - f->base); }

int main(void) {
    /* ---- 1. GDT: канонические значения ---- */
    uint64_t gdt[GDT_ENTRIES];
    gdt_fill(gdt, 0xFFFFFFFF80037000ULL, TSS_SIZE - 1);
    CHECK(gdt[0] == 0);
    CHECK(gdt[SEL_KCODE / 8] == 0x00AF9A000000FFFFULL);   /* совпадает с тем, что было в gdt.c до Фазы 3 */
    CHECK(gdt[SEL_KDATA / 8] == 0x00CF92000000FFFFULL);
    CHECK(gdt[SEL_UDATA_BASE / 8] == 0x00CFF2000000FFFFULL);
    CHECK(gdt[SEL_UCODE_BASE / 8] == 0x00AFFA000000FFFFULL);

    /* ---- 2. GDT: независимый декодер ---- */
    struct dec kc = decode(gdt[1]), kd = decode(gdt[2]), ud = decode(gdt[3]), uc = decode(gdt[4]);
    CHECK(kc.p && kc.s && kc.dpl == 0 && (kc.type & 8) && kc.l && !kc.db && kc.g);   /* код, 64-бит */
    CHECK(kd.p && kd.s && kd.dpl == 0 && !(kd.type & 8) && (kd.type & 2));           /* данные, запись */
    CHECK(uc.p && uc.s && uc.dpl == 3 && (uc.type & 8) && uc.l && !uc.db && uc.g);
    CHECK(ud.p && ud.s && ud.dpl == 3 && !(ud.type & 8) && (ud.type & 2));
    CHECK(uc.limit == 0xFFFFF && ud.limit == 0xFFFFF && uc.base == 0 && ud.base == 0);

    /* ---- 3. TSS-дескриптор: 16 байт, база собирается из кусков ---- */
    struct dec t = decode(gdt[SEL_TSS / 8]);
    uint64_t tbase = (uint64_t)t.base | (gdt[SEL_TSS / 8 + 1] << 32);
    CHECK(t.p && !t.s && t.dpl == 0 && t.type == 9);        /* свободный 64-битный TSS */
    CHECK(tbase == 0xFFFFFFFF80037000ULL);
    CHECK(t.limit == TSS_SIZE - 1);
    CHECK((gdt[SEL_TSS / 8 + 1] >> 32) == 0);                /* зарезервированная половина = 0 */
    CHECK(SEL_TSS / 8 + 2 == GDT_ENTRIES);                   /* TSS занимает ровно последние две записи */
    uint64_t t2[2]; gdt_make_tss_desc(0x0000000012345678ULL, 103, t2);
    struct dec t2d = decode(t2[0]);
    CHECK(t2d.base == 0x12345678u && t2[1] == 0);            /* база с ненулевыми байтами в каждом поле */

    /* ---- 4. Селекторы и совместимость с SYSRET (Фаза 4) ---- */
    CHECK(SEL_UDATA == 0x1B && SEL_UCODE == 0x23);
    CHECK(SEL_KDATA == SEL_KCODE + 8);
    CHECK(SEL_UCODE_BASE == SEL_UDATA_BASE + 8);
    CHECK(SEL_UDATA_BASE == SEL_KDATA + 8);
    {   /* STAR[63:48] = 0x10: SYSRET даёт SS = 0x10+8, CS = 0x10+16 (с RPL=3) */
        unsigned star_sysret = 0x10;
        CHECK((star_sysret + 8) == SEL_UDATA_BASE && (star_sysret + 16) == SEL_UCODE_BASE);
        /* STAR[47:32] = 0x08: SYSCALL даёт CS = 0x08, SS = 0x10 */
        CHECK(SEL_KCODE == 0x08 && SEL_KCODE + 8 == SEL_KDATA);
    }

    /* ---- 5. Раскладка TSS ---- */
    CHECK(sizeof(struct tss64) == TSS_SIZE);
    CHECK(offsetof(struct tss64, rsp0) == 4);
    CHECK(offsetof(struct tss64, ist) == 36);
    CHECK(offsetof(struct tss64, iomap_base) == 102);

    /* ---- 6. Кадр iretq ---- */
    struct iret_frame f;
    CHECK(iret_frame_build(&f, USER_CODE_VA, USER_STACK_TOP) == 0);
    CHECK(f.rip == USER_CODE_VA && f.rsp == USER_STACK_TOP);
    CHECK(f.cs == 0x23 && (f.cs & 3) == 3 && f.ss == 0x1B && (f.ss & 3) == 3);
    CHECK((f.rflags & RFLAGS_IF) && (f.rflags & 2) && !(f.rflags & RFLAGS_IOPL_MASK));
    /* порядок полей = порядок, в котором iretq снимает их со стека = хвост struct regs */
    CHECK(offsetof(struct iret_frame, rip) == 0 && offsetof(struct iret_frame, cs) == 8 &&
          offsetof(struct iret_frame, rflags) == 16 && offsetof(struct iret_frame, rsp) == 24 &&
          offsetof(struct iret_frame, ss) == 32);
    CHECK(offsetof(struct regs, cs) - offsetof(struct regs, rip) == 8);
    CHECK(offsetof(struct regs, rflags) - offsetof(struct regs, rip) == 16);
    CHECK(offsetof(struct regs, rsp) - offsetof(struct regs, rip) == 24);
    CHECK(offsetof(struct regs, ss) - offsetof(struct regs, rip) == 32);
    /* границы: iretq с неканоническим/ядерным rip или rsp дал бы #GP уже в ядре */
    CHECK(iret_frame_build(&f, USER_VA_END - 1, 0x1000) == 0);
    CHECK(iret_frame_build(&f, USER_VA_END, 0x1000) == -1);
    CHECK(iret_frame_build(&f, 0x1000, USER_VA_END) == -1);
    CHECK(iret_frame_build(&f, KERNEL_TEXT_VA, 0x1000) == -1);
    CHECK(iret_frame_build(&f, 0x1000, 0xFFFF800000000000ULL) == -1);
    CHECK(iret_frame_build(&f, 0x0000800000001000ULL, 0x1000) == -1);   /* неканонический */

    /* ---- 7. Код ошибки #PF ---- */
    CHECK((PFE_P | PFE_U) == 0x5);
    CHECK(PFE_W == 0x2 && PFE_RSVD == 0x8 && PFE_I == 0x10);
    CHECK(cs_is_user(0x23) && !cs_is_user(0x08) && cs_is_user(0x1B) && !cs_is_user(0x10));

    /* ---- 8. Раскладка пользовательского адресного пространства ---- */
    CHECK(user_addr_ok(USER_CODE_VA) && user_addr_ok(USER_STACK_VA) && user_addr_ok(USER_STACK_TOP - 1));
    CHECK(!user_addr_ok(KERNEL_TEXT_VA));
    CHECK((USER_CODE_VA & 0xFFF) == 0 && (USER_STACK_VA & 0xFFF) == 0);
    CHECK(USER_CODE_VA + VMM_PAGE <= USER_STACK_VA);
    CHECK((USER_STACK_TOP & 0xF) == 0);                      /* стек 16-выровнен (ABI) */
    {   /* KERNEL_TEXT_VA, который читает программа #2, — действительно адрес образа ядра по linker.ld */
        FILE *fp = fopen("kernel/linker.ld", "r"); char buf[4096] = {0};
        if (fp) { size_t n = fread(buf, 1, sizeof buf - 1, fp); buf[n] = 0; fclose(fp); }
        CHECK(fp && strstr(buf, "0xffffffff80000000"));
    }

    /* ---- 9. Склейка с vmm.c: то, что делает user_run(), на поддельной физпамяти ---- */
    struct fake_phys fp = { calloc(ARENA_PAGES, VMM_PAGE), 0x10000, 0x10000 };
    struct vmm_space ks = { fake_alloc(&fp) };               /* "пространство ядра" (как у Limine) */
    /* у ядра есть страница в верхней половине БЕЗ бита USER — как .text ядра у Limine */
    {   /* ядро строит свои таблицы без USER на листе: пишем напрямую, как это сделал бы загрузчик */
        uint64_t *pml4 = fake_p2v(ks.pml4_phys, &fp);
        uint64_t pdpt = fake_alloc(&fp), pd = fake_alloc(&fp), pt = fake_alloc(&fp), page = fake_alloc(&fp);
        pml4[511] = pdpt | PTE_PRESENT | PTE_WRITE;
        ((uint64_t *)fake_p2v(pdpt, &fp))[510] = pd | PTE_PRESENT | PTE_WRITE;
        ((uint64_t *)fake_p2v(pd, &fp))[0] = pt | PTE_PRESENT | PTE_WRITE;
        ((uint64_t *)fake_p2v(pt, &fp))[0] = page | PTE_PRESENT;            /* только чтение, U=0 */
    }
    uint64_t ks_pml4_before[512]; memcpy(ks_pml4_before, fake_p2v(ks.pml4_phys, &fp), sizeof ks_pml4_before);

    struct vmm_space us = { fake_alloc(&fp) };
    vmm_share_upper_half(&us, &ks, fake_p2v, &fp);
    uint64_t code_pg = fake_alloc(&fp), stk_pg = fake_alloc(&fp);
    CHECK(vmm_map(&us, USER_CODE_VA, code_pg, PTE_USER, fake_alloc, fake_p2v, &fp) == VMM_OK);
    CHECK(vmm_map(&us, USER_STACK_VA, stk_pg, PTE_USER | PTE_WRITE, fake_alloc, fake_p2v, &fp) == VMM_OK);

    uint64_t ph, fl;
    CHECK(vmm_translate(&us, USER_CODE_VA, &ph, &fl, fake_p2v, &fp) == VMM_OK && ph == code_pg);
    CHECK((fl & PTE_USER) && !(fl & PTE_WRITE));             /* код пользователя: читать/исполнять, не писать */
    CHECK(vmm_translate(&us, USER_STACK_VA, &ph, &fl, fake_p2v, &fp) == VMM_OK && ph == stk_pg);
    CHECK((fl & PTE_USER) && (fl & PTE_WRITE));
    /* ring3 проходит страницу, только если бит USER стоит НА ВСЕХ уровнях — проверяем каждый */
    {
        uint64_t va[2] = { USER_CODE_VA, USER_STACK_VA };
        for (int k = 0; k < 2; k++) {
            uint64_t e = ((uint64_t *)fake_p2v(us.pml4_phys, &fp))[(va[k] >> 39) & 0x1FF];
            CHECK((e & PTE_PRESENT) && (e & PTE_USER));
            e = ((uint64_t *)fake_p2v(e & PTE_ADDR_MASK, &fp))[(va[k] >> 30) & 0x1FF];
            CHECK((e & PTE_PRESENT) && (e & PTE_USER));
            e = ((uint64_t *)fake_p2v(e & PTE_ADDR_MASK, &fp))[(va[k] >> 21) & 0x1FF];
            CHECK((e & PTE_PRESENT) && (e & PTE_USER));
        }
    }
    /* ядро видно (страница есть), но у листа нет USER -> из ring3 это #PF с кодом P|U */
    CHECK(vmm_translate(&us, KERNEL_TEXT_VA, &ph, &fl, fake_p2v, &fp) == VMM_OK);
    CHECK(!(fl & PTE_USER));
    /* карта ядра не тронута: пользовательские отображения не пишут в верхнюю половину */
    CHECK(memcmp(ks_pml4_before + VMM_SPLIT_INDEX, (uint64_t *)fake_p2v(ks.pml4_phys, &fp) + VMM_SPLIT_INDEX,
                 (512 - VMM_SPLIT_INDEX) * 8) == 0);
    /* и пользовательские страницы не видны в пространстве ядра */
    CHECK(vmm_translate(&ks, USER_CODE_VA, NULL, NULL, fake_p2v, &fp) == VMM_ERR_NOT_MAPPED);
    CHECK(vmm_translate(&ks, USER_STACK_VA, NULL, NULL, fake_p2v, &fp) == VMM_ERR_NOT_MAPPED);
    /* повторный запуск (user_run переиспользует страницы): повторное отображение безопасно */
    CHECK(vmm_map(&us, USER_CODE_VA, code_pg, PTE_USER, fake_alloc, fake_p2v, &fp) == VMM_OK);

    /* ---- 10. Проверки результата: хороший принимается, любая порча отвергается ---- */
    {
        const uint64_t hoff = 12, poff = 10;
        struct user_result g = { 0, USER_EXIT_FAULT, 13, 0, 0, USER_CODE_VA + hoff, SEL_UCODE, SEL_UDATA,
                                 USER_STACK_TOP, 0x202, USER_MAGIC, USER_MAGIC, 3, 0, 0, 0 };
        CHECK(user_check_hlt(&g, hoff));
        struct user_result b;
        #define BAD(field, val) do { b = g; b.field = (val); CHECK(!user_check_hlt(&b, hoff)); } while (0)
        BAD(setup_err, -4); BAD(exit_kind, USER_EXIT_TIMEOUT); BAD(int_no, 14); BAD(err, 1);
        BAD(rip, USER_CODE_VA); BAD(cs, 0x08); BAD(cs, 0x20); BAD(ss, 0x10); BAD(rsp, USER_STACK_TOP - 8);
        BAD(rax, 0); BAD(rcx, 1); BAD(rflags, 0x2); BAD(rflags, 0x3202);

        struct user_result pf = { 0, USER_EXIT_FAULT, 14, PFE_P | PFE_U, KERNEL_TEXT_VA, USER_CODE_VA + poff,
                                  SEL_UCODE, SEL_UDATA, USER_STACK_TOP, 0x202, 0, 0, 1, 0, 0, 0 };
        CHECK(user_check_pf(&pf, poff));
        #define BADP(field, val) do { b = pf; b.field = (val); CHECK(!user_check_pf(&b, poff)); } while (0)
        BADP(int_no, 13); BADP(err, PFE_U); BADP(err, PFE_P); BADP(err, PFE_P | PFE_U | PFE_W);
        BADP(err, PFE_P | PFE_U | PFE_I); BADP(cr2, 0); BADP(cs, 0x08); BADP(rip, USER_CODE_VA);
        BADP(exit_kind, USER_EXIT_TIMEOUT);

        struct user_result sp = { 0, USER_EXIT_TIMEOUT, 32, 0, 0, USER_CODE_VA, SEL_UCODE, SEL_UDATA,
                                  USER_STACK_TOP, 0x202, 0, 0, 20, 0, 0, 0 };
        CHECK(user_check_spin(&sp, 20));
        #define BADS(field, val) do { b = sp; b.field = (val); CHECK(!user_check_spin(&b, 20)); } while (0)
        BADS(exit_kind, USER_EXIT_FAULT); BADS(cs, 0x08); BADS(rip, USER_CODE_VA + 1); BADS(ticks_in_user, 19);
        BADS(setup_err, -1);
    }

    /* ---- 11. Байты ring3-программ из usermode.S (на хосте только читаем, не исполняем) ---- */
    {
        extern const uint8_t user_prog_hlt_start[], user_prog_hlt_end[], user_prog_pf_start[], user_prog_pf_end[];
        extern const uint8_t user_prog_spin_start[], user_prog_spin_end[];
        extern const uint64_t user_prog_hlt_fault_off, user_prog_pf_fault_off;
        CHECK(user_prog_hlt_fault_off == 12 && user_prog_pf_fault_off == 10);   /* то, что ждут проверки выше */
        CHECK(user_prog_hlt_start[user_prog_hlt_fault_off] == 0xF4);            /* hlt */
        CHECK((uint64_t)(user_prog_hlt_end - user_prog_hlt_start) == user_prog_hlt_fault_off + 1);  /* hlt — последняя */
        CHECK(user_prog_pf_start[user_prog_pf_fault_off] == 0x48 && user_prog_pf_start[user_prog_pf_fault_off + 1] == 0x8B &&
              user_prog_pf_start[user_prog_pf_fault_off + 2] == 0x18);          /* mov rbx,[rax] */
        CHECK((uint64_t)(user_prog_pf_end - user_prog_pf_start) == user_prog_pf_fault_off + 3);
        CHECK(user_prog_spin_end - user_prog_spin_start == 2 && user_prog_spin_start[0] == 0xEB && user_prog_spin_start[1] == 0xFE);
        uint64_t m; memcpy(&m, user_prog_pf_start + 2, 8); CHECK(m == KERNEL_TEXT_VA);   /* читает именно адрес ядра */
        memcpy(&m, user_prog_hlt_start + 2, 8); CHECK(m == USER_MAGIC);
        CHECK((uint64_t)(user_prog_hlt_end - user_prog_hlt_start) < VMM_PAGE);            /* влезает в одну страницу */
    }

    free(fp.arena);
    if (fails) { printf("ПРОВАЛЕНО проверок: %d\n", fails); return 1; }
    printf("gdt/ring3 host: все проверки прошли\n");
    return 0;
}
