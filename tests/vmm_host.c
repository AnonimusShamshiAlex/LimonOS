/* Проверка kernel/vmm.c (Фаза 2, docs/USERSPACE_PLAN.md): отображение,
 * снятие отображения, перевод адреса и "общая верхняя половина" между
 * адресными пространствами ядра и процесса. Настоящая запись в CR3 здесь не
 * участвует вообще (vmm.c её и не вызывает) — вся эта логика чистая, поэтому
 * гоняется на хосте, без эмулятора, как tests/elf_host.c для Фазы 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vmm.h"

static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %d: %s\n", __LINE__, #c); fails++; } }while(0)

/* ---- поддельная физическая память: арена в обычной памяти процесса,
 * "физические адреса" — это смещения от fake_base, как если бы это были
 * настоящие адреса страниц, выданные pmm_alloc(). ---- */
#define ARENA_PAGES 64
struct fake_phys {
    uint8_t *arena;
    uint64_t base;      /* "физический" адрес начала арены */
    uint64_t next;       /* следующая свободная "физическая" страница */
    int      budget;     /* сколько страниц ещё можно выдать (для теста "кончилась память") */
};

static uint64_t fake_alloc(void *ctx) {
    struct fake_phys *fp = ctx;
    if (fp->budget <= 0) return 0;
    uint64_t p = fp->next;
    fp->next += VMM_PAGE;
    fp->budget--;
    return p;
}
static void *fake_p2v(uint64_t phys, void *ctx) {
    struct fake_phys *fp = ctx;
    return fp->arena + (phys - fp->base);
}
static void fake_init(struct fake_phys *fp, int budget) {
    fp->arena = calloc(ARENA_PAGES, VMM_PAGE);
    fp->base = 0x10000;     /* произвольный ненулевой "физический" адрес, как в реальном PMM */
    fp->next = fp->base;
    fp->budget = budget;
}

static struct vmm_space new_space(struct fake_phys *fp) {
    struct vmm_space sp;
    sp.pml4_phys = fake_alloc(fp);
    memset(fake_p2v(sp.pml4_phys, fp), 0, VMM_PAGE);
    return sp;
}

int main(void) {
    struct fake_phys fp; fake_init(&fp, ARENA_PAGES);
    struct vmm_space sp = new_space(&fp);

    /* 1. простое отображение и перевод обратно */
    CHECK(vmm_map(&sp, 0x400000, 0x200000, PTE_WRITE | PTE_USER, fake_alloc, fake_p2v, &fp) == VMM_OK);
    uint64_t ph = 0, fl = 0;
    CHECK(vmm_translate(&sp, 0x400000, &ph, &fl, fake_p2v, &fp) == VMM_OK);
    CHECK(ph == 0x200000);
    CHECK((fl & PTE_WRITE) && (fl & PTE_USER));
    CHECK((fl & PTE_ADDR_MASK) == 0);   /* во флагах не должно остаться бит адреса */

    /* 2. несколько адресов, нарочно раскиданных по разным уровням таблиц:
     *    одна и та же PT, соседняя PD-запись (2 МиБ), соседняя PDPT-запись (1 ГиБ),
     *    соседняя PML4-запись (512 ГиБ) — и все должны жить независимо */
    uint64_t A = 0x1000, B = 0x2000, C = 0x1000 + 0x200000ULL, D = 0x1000 + 0x40000000ULL, E = 0x1000 + 0x8000000000ULL;
    CHECK(vmm_map(&sp, A, 0x300000, PTE_WRITE, fake_alloc, fake_p2v, &fp) == VMM_OK);
    CHECK(vmm_map(&sp, B, 0x301000, PTE_WRITE, fake_alloc, fake_p2v, &fp) == VMM_OK);
    CHECK(vmm_map(&sp, C, 0x302000, PTE_WRITE, fake_alloc, fake_p2v, &fp) == VMM_OK);
    CHECK(vmm_map(&sp, D, 0x303000, PTE_WRITE, fake_alloc, fake_p2v, &fp) == VMM_OK);
    CHECK(vmm_map(&sp, E, 0x304000, PTE_WRITE, fake_alloc, fake_p2v, &fp) == VMM_OK);
    uint64_t pa, pb, pc, pd, pe;
    CHECK(vmm_translate(&sp, A, &pa, NULL, fake_p2v, &fp) == VMM_OK && pa == 0x300000);
    CHECK(vmm_translate(&sp, B, &pb, NULL, fake_p2v, &fp) == VMM_OK && pb == 0x301000);
    CHECK(vmm_translate(&sp, C, &pc, NULL, fake_p2v, &fp) == VMM_OK && pc == 0x302000);
    CHECK(vmm_translate(&sp, D, &pd, NULL, fake_p2v, &fp) == VMM_OK && pd == 0x303000);
    CHECK(vmm_translate(&sp, E, &pe, NULL, fake_p2v, &fp) == VMM_OK && pe == 0x304000);

    /* 3. снимаем B — остальные не должны пострадать */
    CHECK(vmm_unmap(&sp, B, fake_p2v, &fp) == VMM_OK);
    CHECK(vmm_translate(&sp, B, NULL, NULL, fake_p2v, &fp) == VMM_ERR_NOT_MAPPED);
    CHECK(vmm_translate(&sp, A, &pa, NULL, fake_p2v, &fp) == VMM_OK && pa == 0x300000);
    CHECK(vmm_translate(&sp, C, &pc, NULL, fake_p2v, &fp) == VMM_OK && pc == 0x302000);
    CHECK(vmm_unmap(&sp, B, fake_p2v, &fp) == VMM_ERR_NOT_MAPPED);     /* повторный unmap — не крешится, просто "не было" */

    /* 4. невыровненные адреса отклоняются, а не "подрезаются молча" */
    CHECK(vmm_map(&sp, 0x400001, 0x500000, PTE_WRITE, fake_alloc, fake_p2v, &fp) == VMM_ERR_MISALIGNED);
    CHECK(vmm_map(&sp, 0x500000, 0x600001, PTE_WRITE, fake_alloc, fake_p2v, &fp) == VMM_ERR_MISALIGNED);

    /* 5. память "физически" кончилась — alloc_fn не может выдать новую таблицу:
     *    новая область (0..255 не трогали) должна дать VMM_ERR_NOMEM, не падать */
    { struct fake_phys tiny; fake_init(&tiny, 1 /* хватит только на сам PML4 */);
      struct vmm_space s2 = new_space(&tiny);
      CHECK(vmm_map(&s2, 0x10000000, 0x20000, PTE_WRITE, fake_alloc, fake_p2v, &tiny) == VMM_ERR_NOMEM);
      free(tiny.arena); }

    /* 6. общая верхняя половина (VMM_SPLIT_INDEX..511): ядро и "процесс" должны
     *    видеть одни и те же отображения в верхней половине, и эта связь —
     *    живая (через расшаренную таблицу), а не разовая копия */
    struct vmm_space kernel_sp = new_space(&fp);
    struct vmm_space user_sp   = new_space(&fp);
    uint64_t kaddr1 = (300ULL << 39);                 /* index4 = 300, это >= VMM_SPLIT_INDEX (256) — "ядерная" половина */
    CHECK(vmm_map(&kernel_sp, kaddr1, 0x700000, PTE_WRITE, fake_alloc, fake_p2v, &fp) == VMM_OK);
    CHECK(vmm_translate(&user_sp, kaddr1, NULL, NULL, fake_p2v, &fp) == VMM_ERR_NOT_MAPPED); /* до share — не видно */

    vmm_share_upper_half(&user_sp, &kernel_sp, fake_p2v, &fp);
    uint64_t pk1; CHECK(vmm_translate(&user_sp, kaddr1, &pk1, NULL, fake_p2v, &fp) == VMM_OK && pk1 == 0x700000);

    /* живая связь: новое отображение в том же top-level индексе (300), но
     * другая PDPT-запись — появляется в user_sp само, без повторного share */
    uint64_t kaddr2 = (300ULL << 39) | (1ULL << 30);
    CHECK(vmm_map(&kernel_sp, kaddr2, 0x701000, PTE_WRITE, fake_alloc, fake_p2v, &fp) == VMM_OK);
    uint64_t pk2; CHECK(vmm_translate(&user_sp, kaddr2, &pk2, NULL, fake_p2v, &fp) == VMM_OK && pk2 == 0x701000);

    /* а нижняя половина (пользовательская) остаётся независимой */
    uint64_t uaddr = (5ULL << 39);                     /* index4 = 5, < VMM_SPLIT_INDEX */
    CHECK(vmm_map(&user_sp, uaddr, 0x800000, PTE_WRITE | PTE_USER, fake_alloc, fake_p2v, &fp) == VMM_OK);
    CHECK(vmm_translate(&kernel_sp, uaddr, NULL, NULL, fake_p2v, &fp) == VMM_ERR_NOT_MAPPED);

    free(fp.arena);
    printf(fails ? "FAILED %d\n" : "VMM OK\n", fails);
    return fails ? 1 : 0;
}
