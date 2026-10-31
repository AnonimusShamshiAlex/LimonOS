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

/* ---- куча: список блоков, первый подходящий, склейка соседей ----
   Защита: у каждого блока есть метка (magic). kfree проверяет, что указатель настоящий и
   не освобождён дважды; kmalloc не даёт переполнить размер; когда кучи не хватает — берёт
   у менеджера страниц ещё один кусок (до HEAP_MAX_CHUNKS). */
#define BLK_MAGIC_USED 0xA110C0DEA110C0DEULL
#define BLK_MAGIC_FREE 0xF4EEF4EEF4EEF4EEULL
#define HEAP_MAX_CHUNKS 16
struct blk { size_t size; size_t free; struct blk *next; uint64_t magic; } __attribute__((aligned(16)));
static struct blk *head, *tail;
static unsigned heap_chunks;
static unsigned heap_bad_free;       /* сколько раз ловили плохой/двойной kfree */

static void chunk_add(uint64_t phys, size_t pages) {
    struct blk *b = phys_to_virt(phys);
    b->size = pages * 4096 - sizeof(struct blk);
    b->free = 1; b->next = NULL; b->magic = BLK_MAGIC_FREE;
    if (tail) tail->next = b; else head = b;
    tail = b;
    heap_chunks++;
}

void heap_init(size_t pages) {
    uint64_t p = pmm_alloc(pages);
    if (!p) return;
    chunk_add(p, pages);
}

static int heap_grow(size_t need) {
    if (heap_chunks >= HEAP_MAX_CHUNKS) return 0;
    size_t pages = (need + sizeof(struct blk) + 4095) / 4096;
    if (pages < 1024) pages = 1024;                           /* кусок не меньше 4 МиБ */
    uint64_t p = pmm_alloc(pages);
    if (!p) { pages = (need + sizeof(struct blk) + 4095) / 4096; p = pmm_alloc(pages); }
    if (!p) return 0;
    chunk_add(p, pages);
    return 1;
}

static void *kmalloc_try(size_t n) {
    for (struct blk *b = head; b; b = b->next) {
        if (!b->free || b->size < n) continue;
        if (b->size >= n + sizeof(struct blk) + 16) {
            struct blk *rest = (struct blk *)((uint8_t *)(b + 1) + n);
            rest->size = b->size - n - sizeof(struct blk);
            rest->free = 1; rest->magic = BLK_MAGIC_FREE;
            rest->next = b->next;
            b->next = rest;
            if (tail == b) tail = rest;
            b->size = n;
        }
        b->free = 0; b->magic = BLK_MAGIC_USED;
        return b + 1;
    }
    return NULL;
}

void *kmalloc(size_t n) {
    if (n > ((size_t)1 << 40)) return NULL;                   /* заведомо нереальный размер: иначе n+15 переполнится */
    n = (n + 15) & ~(size_t)15;
    if (n == 0) n = 16;
    void *p = kmalloc_try(n);
    if (!p && heap_grow(n)) p = kmalloc_try(n);
    return p;
}

void *kcalloc(size_t cnt, size_t sz) {
    if (sz && cnt > ((size_t)1 << 40) / sz) return NULL;      /* cnt*sz не переполняется */
    size_t n = cnt * sz;
    void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void kfree(void *p) {
    if (!p) return;
    struct blk *b = (struct blk *)p - 1;
    /* указатель должен лежать ровно на заголовке какого-то блока нашей кучи */
    int found = 0;
    for (struct blk *c = head; c; c = c->next) if (c == b) { found = 1; break; }
    if (!found || b->magic != BLK_MAGIC_USED || b->free) { heap_bad_free++; return; }
    b->free = 1; b->magic = BLK_MAGIC_FREE;
    for (struct blk *c = head; c; c = c->next)
        while (c->free && c->next && c->next->free &&
               (uint8_t *)(c + 1) + c->size == (uint8_t *)c->next) {     /* склеиваем только физических соседей */
            if (tail == c->next) tail = c;
            c->size += sizeof(struct blk) + c->next->size;
            c->next = c->next->next;
        }
}

size_t heap_used(void) {
    size_t u = 0;
    for (struct blk *b = head; b; b = b->next) if (!b->free) u += b->size;
    return u;
}

size_t heap_total(void) {
    size_t t = 0;
    for (struct blk *b = head; b; b = b->next) t += b->size + sizeof(struct blk);
    return t;
}

unsigned heap_bad_frees(void) { return heap_bad_free; }
