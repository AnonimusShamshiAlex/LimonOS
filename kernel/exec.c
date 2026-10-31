#include "exec.h"

#define PAGE 4096ULL

struct page_rec { uint64_t va; int writable; uint8_t *mem; };

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

enum exec_err exec_load(const struct elf64_info *info, const uint8_t *file, size_t len, const struct exec_ops *ops) {
    struct page_rec pg[EXEC_MAX_PAGES];
    size_t np = 0;

    /* 1) проверка диапазонов и сбор уникальных страниц вместе с итоговым правом записи */
    for (int s = 0; s < info->nseg; s++) {
        const struct elf64_segment *g = &info->seg[s];
        if (g->memsz == 0) continue;
        if (g->vaddr < EXEC_IMAGE_LO || g->vaddr >= EXEC_IMAGE_HI) return EXEC_ERR_RANGE;
        if (g->memsz > EXEC_IMAGE_HI - g->vaddr) return EXEC_ERR_RANGE;            /* без переполнения */
        if (g->filesz > g->memsz) return EXEC_ERR_RANGE;
        if (g->fileoff > len || g->filesz > len - g->fileoff) return EXEC_ERR_RANGE;
        uint64_t first = g->vaddr & ~(PAGE - 1), last = (g->vaddr + g->memsz - 1) & ~(PAGE - 1);
        for (uint64_t va = first; va <= last; va += PAGE) {
            size_t k;
            for (k = 0; k < np; k++) if (pg[k].va == va) break;
            if (k == np) {
                if (np == EXEC_MAX_PAGES) return EXEC_ERR_TOO_BIG;
                pg[np].va = va; pg[np].writable = 0; pg[np].mem = 0; np++;
            }
            if (g->flags & PF_W) pg[k].writable = 1;
        }
    }
    /* 2) создать страницы */
    for (size_t k = 0; k < np; k++) {
        pg[k].mem = ops->page(ops->ctx, pg[k].va, pg[k].writable);
        if (!pg[k].mem) return EXEC_ERR_NOMEM;
    }
    /* 3) скопировать файловую часть сегментов; всё остальное (bss, хвосты страниц) уже нули */
    for (int s = 0; s < info->nseg; s++) {
        const struct elf64_segment *g = &info->seg[s];
        for (uint64_t i = 0; i < g->filesz; i++) {
            uint64_t va = g->vaddr + i, base = va & ~(PAGE - 1);
            size_t k = 0;
            while (pg[k].va != base) k++;                    /* страница гарантированно есть (шаг 1) */
            pg[k].mem[va - base] = file[g->fileoff + i];
        }
    }
    return EXEC_OK;
}

enum exec_err exec_build_stack(uint8_t *top_page, uint64_t top_va, int argc, const char *const *argv, uint64_t *rsp) {
    if (argc < 0 || argc > EXEC_MAX_ARGS) return EXEC_ERR_ARGS;
    uint64_t base_va = top_va - PAGE;
    uint64_t sp = top_va;
    uint64_t str_va[EXEC_MAX_ARGS];
    /* строки аргументов — у самой вершины, с конца к началу */
    for (int i = argc - 1; i >= 0; i--) {
        size_t n = slen(argv[i]) + 1;
        if (n > sp - base_va) return EXEC_ERR_ARGS;
        sp -= n;
        for (size_t k = 0; k < n; k++) top_page[sp - base_va + k] = (uint8_t)argv[i][k];
        str_va[i] = sp;
    }
    /* дальше: argc, argv[argc+1], envp[1] (только NULL), auxv[2] (AT_NULL) */
    uint64_t words = 1 + (uint64_t)argc + 1 + 1 + 2;
    uint64_t need = words * 8;
    sp &= ~15ULL;
    if (need + 16 > sp - base_va) return EXEC_ERR_ARGS;
    sp = (sp - need) & ~15ULL;                                /* rsp кратен 16 при входе в _start */
    uint64_t *w = (uint64_t *)(top_page + (sp - base_va));
    size_t j = 0;
    w[j++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) w[j++] = str_va[i];
    w[j++] = 0;                                               /* конец argv */
    w[j++] = 0;                                               /* конец envp (окружения нет) */
    w[j++] = 0; w[j++] = 0;                                   /* auxv: AT_NULL */
    *rsp = sp;
    return EXEC_OK;
}

const char *exec_err_str(enum exec_err e) {
    switch (e) {
    case EXEC_OK:           return "ок";
    case EXEC_ERR_RANGE:    return "сегмент программы вне допустимых адресов (0x400000..0x10000000)";
    case EXEC_ERR_TOO_BIG:  return "программа слишком большая (максимум 256 КиБ)";
    case EXEC_ERR_NOMEM:    return "не хватило памяти под программу";
    case EXEC_ERR_ARGS:     return "аргументы не помещаются в стек";
    }
    return "неизвестная ошибка";
}
