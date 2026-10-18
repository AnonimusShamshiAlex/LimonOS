#include "mem.h"
#include "kstring.h"

#define MAX_REGIONS 48

struct region { uint64_t base, size, used; };
static struct region regions[MAX_REGIONS];
static int nregions;
static uint64_t hhdm;
static uint64_t total_alloc;

void mem_init(uint64_t hhdm_offset, struct limine_memmap_response *m) {
    hhdm = hhdm_offset;
    if (!m) return;
    for (uint64_t i = 0; i < m->entry_count && nregions < MAX_REGIONS; i++) {
        struct limine_memmap_entry *e = m->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t s = (e->base + 0xFFF) & ~0xFFFULL;
        uint64_t en = (e->base + e->length) & ~0xFFFULL;
        if (s < 0x100000) s = 0x100000;           /* нижний мегабайт не трогаем */
        if (en <= s) continue;
        regions[nregions].base = s;
        regions[nregions].size = en - s;
        regions[nregions].used = 0;
        nregions++;
    }
}

void *phys_to_virt(uint64_t phys) { return (void *)(phys + hhdm); }

uint64_t pmm_alloc(size_t pages) {
    uint64_t sz = (uint64_t)pages * 4096;
    for (int pass = 0; pass < 2; pass++) {            /* сначала ищем ниже 4 ГиБ (удобно для DMA) */
        for (int i = 0; i < nregions; i++) {
            struct region *r = &regions[i];
            if (r->size - r->used < sz) continue;
            uint64_t a = r->base + r->used;
            if (pass == 0 && a + sz > 0x100000000ULL) continue;
            r->used += sz;
            total_alloc += sz;
            memset(phys_to_virt(a), 0, sz);
            return a;
        }
    }
    return 0;
}

uint64_t pmm_free_bytes(void) {
    uint64_t f = 0;
    for (int i = 0; i < nregions; i++) f += regions[i].size - regions[i].used;
    return f;
}
uint64_t pmm_used_bytes(void) { return total_alloc; }

/* ---- куча: список блоков, первый подходящий, склейка соседей ---- */
struct blk { size_t size; size_t free; struct blk *next; } __attribute__((aligned(16)));
static struct blk *head;

void heap_init(size_t pages) {
    uint64_t p = pmm_alloc(pages);
    if (!p) return;
    head = phys_to_virt(p);
    head->size = pages * 4096 - sizeof(struct blk);
    head->free = 1;
    head->next = NULL;
}

void *kmalloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (n == 0) n = 16;
    for (struct blk *b = head; b; b = b->next) {
        if (!b->free || b->size < n) continue;
        if (b->size >= n + sizeof(struct blk) + 16) {
            struct blk *rest = (struct blk *)((uint8_t *)(b + 1) + n);
            rest->size = b->size - n - sizeof(struct blk);
            rest->free = 1;
            rest->next = b->next;
            b->next = rest;
            b->size = n;
        }
        b->free = 0;
        return b + 1;
    }
    return NULL;
}

void kfree(void *p) {
    if (!p) return;
    ((struct blk *)p - 1)->free = 1;
    for (struct blk *c = head; c; c = c->next)
        while (c->free && c->next && c->next->free) {
            c->size += sizeof(struct blk) + c->next->size;
            c->next = c->next->next;
        }
}

size_t heap_used(void) {
    size_t u = 0;
    for (struct blk *b = head; b; b = b->next) if (!b->free) u += b->size;
    return u;
}
