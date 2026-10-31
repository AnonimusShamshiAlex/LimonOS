#ifndef LIMON_VMM_H
#define LIMON_VMM_H
#include <stdint.h>
#include <stddef.h>

/*
 * Фаза 2 плана запуска настоящих программ (docs/USERSPACE_PLAN.md):
 * свои таблицы страниц на процесс поверх общего ядра/HHDM.
 *
 * Два слоя в этом файле, и это специально:
 *
 *  1) "Чистая" часть (vmm_map/vmm_unmap/vmm_translate/vmm_share_upper_half) —
 *     работает только через переданные ей функции аллокации страницы и
 *     перевода физ->вирт (vmm_alloc_page_fn/vmm_p2v_fn). Она не трогает CR3,
 *     не содержит ни одной asm-инструкции и поэтому честно протестирована на
 *     хосте (tests/vmm_host.c) с поддельной физической памятью — как
 *     kernel/elf.c в Фазе 1.
 *
 *  2) "Железная" часть (vmm_read_cr3/vmm_load_cr3/vmm_invlpg) — настоящие
 *     cr3/invlpg. Она всего в несколько строк и написана строго по
 *     спецификации x86_64, но в ЭТОЙ среде (контейнер без KVM/QEMU и без
 *     сети, чтобы его скачать) её физически нечем проверить — ни на
 *     реальном железе, ни в эмуляторе. Это тот же случай, что RTL8139/ATA в
 *     1.1: написано по спеке, не запускалось. С Фазы 3 её зовёт
 *     kernel/user.c (user_run) — только по команде `ring3`, в обычной работе ядра
 *     CR3 по-прежнему не трогается; первая проверка — на эмуляторе/железе у тебя.
 */

#define VMM_PAGE 4096ULL

/* Биты записи таблицы страниц x86_64 (стандарт, Intel SDM vol.3 §4.5) */
#define PTE_PRESENT 0x001ULL
#define PTE_WRITE   0x002ULL
#define PTE_USER    0x004ULL
#define PTE_ACCESSED 0x020ULL
#define PTE_DIRTY   0x040ULL
#define PTE_NX      0x8000000000000000ULL   /* исполнение запрещено; требует EFER.NXE */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* Индекс 256 делитканонические 48-битное адресное пространство пополам:
 * 0..255 — пользовательская половина (0x0000000000000000..0x00007FFFFFFFFFFF),
 * 256..511 — верхняя половина ядра/HHDM (0xFFFF800000000000 и выше).
 * Так делают Linux и большинство hobby-ядер: верхние 256 записей PML4
 * одинаковы (разделяются) во всех процессах, нижние 256 — свои. */
#define VMM_SPLIT_INDEX 256

/* Аллоцирует одну физическую страницу (4 КиБ, обнулённую) под новую таблицу
 * страниц; возвращает физический адрес или 0, если памяти нет. ctx — то, что
 * передали в vmm_map/vmm_new_space (например, указатель на менеджер кучи). */
typedef uint64_t (*vmm_alloc_page_fn)(void *ctx);

/* Переводит физический адрес в адрес, по которому код ядра может читать/писать
 * эту память прямо сейчас (в реальном ядре — через HHDM, phys_to_virt()). */
typedef void *(*vmm_p2v_fn)(uint64_t phys, void *ctx);

struct vmm_space {
    uint64_t pml4_phys;   /* физический адрес корневой таблицы (PML4) этого адресного пространства */
};

enum vmm_err {
    VMM_OK = 0,
    VMM_ERR_NOMEM,        /* alloc_fn вернула 0 при создании промежуточной таблицы */
    VMM_ERR_NOT_MAPPED,   /* vmm_unmap/vmm_translate: записи нет */
    VMM_ERR_MISALIGNED,   /* vaddr или phys не выровнены на 4 КиБ */
};

/* Отображает одну страницу vaddr -> phys с флагами flags (PTE_WRITE/PTE_USER/PTE_NX;
 * PTE_PRESENT добавляется всегда). При необходимости создаёт промежуточные
 * таблицы (PDPT/PD/PT) через alloc_fn. Перезаписывает существующее отображение,
 * если оно уже было. */
enum vmm_err vmm_map(struct vmm_space *sp, uint64_t vaddr, uint64_t phys, uint64_t flags,
                      vmm_alloc_page_fn alloc_fn, vmm_p2v_fn p2v_fn, void *ctx);

/* Снимает отображение одной страницы. VMM_ERR_NOT_MAPPED, если её не было
 * (включая случай, когда не создана даже промежуточная таблица). */
enum vmm_err vmm_unmap(struct vmm_space *sp, uint64_t vaddr, vmm_p2v_fn p2v_fn, void *ctx);

/* Переводит vaddr в физический адрес и (если out_flags не NULL) флаги записи.
 * Ничего не создаёт — если по пути нет таблицы, это VMM_ERR_NOT_MAPPED. */
enum vmm_err vmm_translate(struct vmm_space *sp, uint64_t vaddr, uint64_t *out_phys,
                            uint64_t *out_flags, vmm_p2v_fn p2v_fn, void *ctx);

/* Копирует верхние 256 записей PML4 (индексы VMM_SPLIT_INDEX..511, то есть
 * ядро+HHDM) из kernel_space в dst. После этого отображения в "пользовательской"
 * половине (0..255) двух пространств независимы, а в "ядерной" половине —
 * общие: страницы ядра видны процессу в ring0 одинаково, какой бы CR3 ни
 * был загружен. Сами 256 записей не копируются глубже — таблицы следующего
 * уровня для них ОБЩИЕ (расшаренные), это и даёт нужный эффект. */
void vmm_share_upper_half(struct vmm_space *dst, const struct vmm_space *kernel_space,
                           vmm_p2v_fn p2v_fn, void *ctx);

/* ---- "железная" часть: реальные cr3/invlpg. См. комментарий сверху файла:
 * нигде пока не вызывается из остального ядра, написана по спецификации,
 * не проверена на реальном/эмулируемом железе в этой среде. */
#ifdef HOST_TEST
static inline uint64_t vmm_read_cr3(void) { return 0; }
static inline void vmm_load_cr3(uint64_t phys) { (void)phys; }
static inline void vmm_invlpg(uint64_t vaddr) { (void)vaddr; }
#else
static inline uint64_t vmm_read_cr3(void) {
    uint64_t v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v;
}
static inline void vmm_load_cr3(uint64_t phys) {
    __asm__ volatile ("mov %0, %%cr3" :: "r"(phys) : "memory");
}
static inline void vmm_invlpg(uint64_t vaddr) {
    __asm__ volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");
}
#endif

#endif
