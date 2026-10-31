#include "libc.h"
/* malloc поверх brk: список блоков с заголовком, первый подходящий, слияние со следующим свободным.
 * Простой и предсказуемый — не быстрый. Куча процесса в LimonOS ограничена ядром (сейчас 64 КиБ). */
struct blk { size_t size; int free; struct blk *next; };
#define HDR ((sizeof(struct blk) + 15) & ~(size_t)15)

static struct blk *head, *tail;

static struct blk *grow(size_t need) {
    char *cur = sbrk_raw(0);
    if ((long)cur < 0) return 0;
    size_t total = HDR + need;
    char *want = cur + total;
    char *got = sbrk_raw(want);
    if (got != want) return 0;                 /* ядро не дало столько кучи */
    struct blk *b = (struct blk *)cur;
    b->size = need; b->free = 0; b->next = 0;
    if (tail) tail->next = b; else head = b;
    tail = b;
    return b;
}

void *malloc(size_t n) {
    if (n == 0) n = 1;
    n = (n + 15) & ~(size_t)15;
    for (struct blk *b = head; b; b = b->next) {
        if (!b->free || b->size < n) continue;
        if (b->size >= n + HDR + 16) {         /* делим блок, остаток остаётся свободным */
            struct blk *r = (struct blk *)((char *)b + HDR + n);
            r->size = b->size - n - HDR; r->free = 1; r->next = b->next;
            b->size = n; b->next = r;
            if (tail == b) tail = r;
        }
        b->free = 0;
        return (char *)b + HDR;
    }
    struct blk *b = grow(n);
    return b ? (char *)b + HDR : 0;
}

void free(void *p) {
    if (!p) return;
    struct blk *b = (struct blk *)((char *)p - HDR);
    b->free = 1;
    while (b->next && b->next->free) {         /* слияние вперёд */
        struct blk *n = b->next;
        b->size += HDR + n->size; b->next = n->next;
        if (tail == n) tail = b;
    }
}

void *calloc(size_t cnt, size_t sz) {
    if (sz && cnt > (size_t)-1 / sz) return 0;   /* переполнение cnt*sz */
    void *p = malloc(cnt * sz);
    if (p) memset(p, 0, cnt * sz);
    return p;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (n == 0) { free(p); return 0; }
    struct blk *b = (struct blk *)((char *)p - HDR);
    if (b->size >= n) return p;
    void *q = malloc(n);
    if (!q) return 0;
    memcpy(q, p, b->size);
    free(p);
    return q;
}
