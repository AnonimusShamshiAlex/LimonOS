#include "vmm.h"
#include "kstring.h"

static inline unsigned idx4(uint64_t va) { return (unsigned)((va >> 39) & 0x1FF); }
static inline unsigned idx3(uint64_t va) { return (unsigned)((va >> 30) & 0x1FF); }
static inline unsigned idx2(uint64_t va) { return (unsigned)((va >> 21) & 0x1FF); }
static inline unsigned idx1(uint64_t va) { return (unsigned)((va >> 12) & 0x1FF); }

static inline uint64_t *table_at(uint64_t phys, vmm_p2v_fn p2v_fn, void *ctx) {
    return (uint64_t *)p2v_fn(phys, ctx);
}

/* Находит физический адрес таблицы следующего уровня по записи table[idx].
 * Если её ещё нет и create==1, аллоцирует новую (обнулённую) страницу через
 * alloc_fn и прописывает запись как PRESENT|WRITE|USER (разрешения сужаются
 * только на последнем уровне, в самой PTE — так принято: промежуточные
 * уровни просто "пропускают", реальные права — у листа). Возвращает 0, если
 * таблицы нет и create==0, либо если alloc_fn не смогла выделить память. */
static uint64_t step(uint64_t *table, unsigned idx, int create,
                      vmm_alloc_page_fn alloc_fn, vmm_p2v_fn p2v_fn, void *ctx) {
    uint64_t e = table[idx];
    if (e & PTE_PRESENT) return e & PTE_ADDR_MASK;
    if (!create) return 0;
    uint64_t np = alloc_fn(ctx);
    if (!np) return 0;
    memset(table_at(np, p2v_fn, ctx), 0, VMM_PAGE);
    table[idx] = (np & PTE_ADDR_MASK) | PTE_PRESENT | PTE_WRITE | PTE_USER;
    return np;
}

static int aligned(uint64_t x) { return (x & (VMM_PAGE - 1)) == 0; }

enum vmm_err vmm_map(struct vmm_space *sp, uint64_t vaddr, uint64_t phys, uint64_t flags,
                      vmm_alloc_page_fn alloc_fn, vmm_p2v_fn p2v_fn, void *ctx) {
    if (!aligned(vaddr) || !aligned(phys)) return VMM_ERR_MISALIGNED;

    uint64_t *pml4 = table_at(sp->pml4_phys, p2v_fn, ctx);
    uint64_t pdpt_p = step(pml4, idx4(vaddr), 1, alloc_fn, p2v_fn, ctx);
    if (!pdpt_p) return VMM_ERR_NOMEM;
    uint64_t *pdpt = table_at(pdpt_p, p2v_fn, ctx);
    uint64_t pd_p = step(pdpt, idx3(vaddr), 1, alloc_fn, p2v_fn, ctx);
    if (!pd_p) return VMM_ERR_NOMEM;
    uint64_t *pd = table_at(pd_p, p2v_fn, ctx);
    uint64_t pt_p = step(pd, idx2(vaddr), 1, alloc_fn, p2v_fn, ctx);
    if (!pt_p) return VMM_ERR_NOMEM;
    uint64_t *pt = table_at(pt_p, p2v_fn, ctx);

    pt[idx1(vaddr)] = (phys & PTE_ADDR_MASK) | (flags & ~PTE_ADDR_MASK) | PTE_PRESENT;
    return VMM_OK;
}

enum vmm_err vmm_unmap(struct vmm_space *sp, uint64_t vaddr, vmm_p2v_fn p2v_fn, void *ctx) {
    if (!aligned(vaddr)) return VMM_ERR_MISALIGNED;
    uint64_t *pml4 = table_at(sp->pml4_phys, p2v_fn, ctx);
    uint64_t pdpt_p = step(pml4, idx4(vaddr), 0, NULL, p2v_fn, ctx);
    if (!pdpt_p) return VMM_ERR_NOT_MAPPED;
    uint64_t *pdpt = table_at(pdpt_p, p2v_fn, ctx);
    uint64_t pd_p = step(pdpt, idx3(vaddr), 0, NULL, p2v_fn, ctx);
    if (!pd_p) return VMM_ERR_NOT_MAPPED;
    uint64_t *pd = table_at(pd_p, p2v_fn, ctx);
    uint64_t pt_p = step(pd, idx2(vaddr), 0, NULL, p2v_fn, ctx);
    if (!pt_p) return VMM_ERR_NOT_MAPPED;
    uint64_t *pt = table_at(pt_p, p2v_fn, ctx);

    if (!(pt[idx1(vaddr)] & PTE_PRESENT)) return VMM_ERR_NOT_MAPPED;
    pt[idx1(vaddr)] = 0;
    return VMM_OK;
}

enum vmm_err vmm_translate(struct vmm_space *sp, uint64_t vaddr, uint64_t *out_phys,
                            uint64_t *out_flags, vmm_p2v_fn p2v_fn, void *ctx) {
    uint64_t *pml4 = table_at(sp->pml4_phys, p2v_fn, ctx);
    uint64_t pdpt_p = step(pml4, idx4(vaddr), 0, NULL, p2v_fn, ctx);
    if (!pdpt_p) return VMM_ERR_NOT_MAPPED;
    uint64_t *pdpt = table_at(pdpt_p, p2v_fn, ctx);
    uint64_t pd_p = step(pdpt, idx3(vaddr), 0, NULL, p2v_fn, ctx);
    if (!pd_p) return VMM_ERR_NOT_MAPPED;
    uint64_t *pd = table_at(pd_p, p2v_fn, ctx);
    uint64_t pt_p = step(pd, idx2(vaddr), 0, NULL, p2v_fn, ctx);
    if (!pt_p) return VMM_ERR_NOT_MAPPED;
    uint64_t *pt = table_at(pt_p, p2v_fn, ctx);

    uint64_t e = pt[idx1(vaddr)];
    if (!(e & PTE_PRESENT)) return VMM_ERR_NOT_MAPPED;
    if (out_phys) *out_phys = e & PTE_ADDR_MASK;
    if (out_flags) *out_flags = e & ~PTE_ADDR_MASK;
    return VMM_OK;
}

void vmm_share_upper_half(struct vmm_space *dst, const struct vmm_space *kernel_space,
                           vmm_p2v_fn p2v_fn, void *ctx) {
    uint64_t *d = table_at(dst->pml4_phys, p2v_fn, ctx);
    const uint64_t *k = table_at(kernel_space->pml4_phys, p2v_fn, ctx);
    for (int i = VMM_SPLIT_INDEX; i < 512; i++) d[i] = k[i];
}
