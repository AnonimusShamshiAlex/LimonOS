/* Проверка kernel/exec.c (Фаза 5) на настоящих ELF-файлах, собранных gcc+ld из userland/.
 * argv[1] — hello.elf для LimonOS (int 0x80), argv[2] — тот же код, собранный с `syscall` для Linux.
 *
 * Сквозная проверка: образ hello_host.elf загружается НАШИМ exec_load в страницы, отображённые mmap по тем же
 * адресам, что в ELF, стек строится НАШИМ exec_build_stack, и управление передаётся на e_entry в дочернем
 * процессе. Если загрузчик что-то положил не туда, программа упадёт или напечатает не то.
 * НЕ проверяется: исполнение именно LimonOS-варианта (int 0x80) на процессоре без Linux — это QEMU/железо. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "exec.h"

static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %d: %s\n", __LINE__, #c); fails++; } }while(0)

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) { printf("нет файла %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END); *len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len); if (fread(b, 1, *len, f) != *len) exit(2); fclose(f); return b;
}

/* ---- ops: «страницы» из malloc, с учётом, что где отображено ---- */
struct rec { uint64_t va; int w; uint8_t *mem; };
struct fake { struct rec r[128]; int n; int fail_at; };
static uint8_t *fake_page(void *c, uint64_t va, int w) {
    struct fake *f = c;
    if (f->n == f->fail_at) return NULL;
    for (int i = 0; i < f->n; i++) if (f->r[i].va == va) { printf("FAIL: страница %llx создана дважды\n", (unsigned long long)va); fails++; }
    f->r[f->n].va = va; f->r[f->n].w = w;
    f->r[f->n].mem = aligned_alloc(4096, 4096); memset(f->r[f->n].mem, 0, 4096);
    return f->r[f->n++].mem;
}
static void fake_free(struct fake *f) { for (int i = 0; i < f->n; i++) free(f->r[i].mem); f->n = 0; }
static struct rec *find(struct fake *f, uint64_t va) { for (int i = 0; i < f->n; i++) if (f->r[i].va == (va & ~4095ULL)) return &f->r[i]; return 0; }

/* ---- ops для сквозного запуска: настоящие страницы по тем же адресам ---- */
static uint8_t *mmap_page(void *c, uint64_t va, int w) {
    (void)c; (void)w;
    void *p = mmap((void *)va, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    return p == MAP_FAILED || (uint64_t)p != va ? NULL : p;
}

int main(int argc, char **argv) {
    if (argc < 3) { printf("usage\n"); return 2; }
    size_t len; uint8_t *elf = slurp(argv[1], &len);
    struct elf64_info info; CHECK(elf64_parse(elf, len, &info) == ELF_OK);
    CHECK(info.nseg == 2);

    /* ---- загрузка настоящего hello.elf ---- */
    struct fake f = { .fail_at = -1 }; struct exec_ops ops = { &f, fake_page };
    CHECK(exec_load(&info, elf, len, &ops) == EXEC_OK);
    for (int s = 0; s < info.nseg; s++) {
        const struct elf64_segment *g = &info.seg[s];
        int ok = 1;
        for (uint64_t i = 0; i < g->memsz; i++) {
            struct rec *r = find(&f, g->vaddr + i); if (!r) { ok = 0; break; }
            uint8_t got = r->mem[(g->vaddr + i) & 4095];
            uint8_t want = i < g->filesz ? elf[g->fileoff + i] : 0;       /* файловая часть — байт в байт, хвост — нули */
            if (got != want) { ok = 0; break; }
        }
        CHECK(ok);
        struct rec *r = find(&f, g->vaddr);
        CHECK(r && r->w == ((g->flags & PF_W) ? 1 : 0));                  /* код не записываемый, данные — да */
    }
    CHECK(find(&f, info.entry) && !find(&f, info.entry)->w);              /* точка входа — в коде */
    CHECK(info.entry >= EXEC_IMAGE_LO && info.entry < EXEC_IMAGE_HI);
    fake_free(&f);

    /* ---- ошибки загрузки на искусственных сегментах ---- */
    uint8_t file[0x2000]; for (size_t i = 0; i < sizeof file; i++) file[i] = (uint8_t)(i * 7 + 1);
    struct elf64_info t; memset(&t, 0, sizeof t); t.nseg = 1;
    #define SEG(i, va, ms, fo, fs, fl) do { t.seg[i].vaddr = (va); t.seg[i].memsz = (ms); t.seg[i].fileoff = (fo); t.seg[i].filesz = (fs); t.seg[i].flags = (fl); } while (0)
    SEG(0, 0x3FF000, 0x1000, 0, 0x100, PF_R | PF_X);          CHECK(exec_load(&t, file, sizeof file, &ops) == EXEC_ERR_RANGE);   /* ниже образа */
    SEG(0, EXEC_IMAGE_HI, 0x1000, 0, 0x100, PF_R | PF_X);      CHECK(exec_load(&t, file, sizeof file, &ops) == EXEC_ERR_RANGE);   /* в куче */
    SEG(0, EXEC_IMAGE_HI - 0x1000, 0x2000, 0, 0x100, PF_R);    CHECK(exec_load(&t, file, sizeof file, &ops) == EXEC_ERR_RANGE);   /* заходит в кучу */
    SEG(0, 0x400000, 0xFFFFFFFFFFFFF000ULL, 0, 0, PF_R);       CHECK(exec_load(&t, file, sizeof file, &ops) == EXEC_ERR_RANGE);   /* переполнение */
    SEG(0, 0x400000, 0x1000, 0x1F00, 0x200, PF_R);             CHECK(exec_load(&t, file, sizeof file, &ops) == EXEC_ERR_RANGE);   /* за концом файла */
    SEG(0, 0x400000, 0x10, 0, 0x100, PF_R);                    CHECK(exec_load(&t, file, sizeof file, &ops) == EXEC_ERR_RANGE);   /* filesz > memsz */
    SEG(0, 0x400000, 0x41000, 0, 0, PF_R | PF_W);              CHECK(exec_load(&t, file, sizeof file, &ops) == EXEC_ERR_TOO_BIG); /* 65 страниц */
    fake_free(&f);
    SEG(0, 0x400000, 0x40000, 0, 0, PF_R | PF_W);              CHECK(exec_load(&t, file, sizeof file, &ops) == EXEC_OK && f.n == 64); fake_free(&f);  /* ровно предел */
    /* нехватка памяти на второй странице */
    SEG(0, 0x400000, 0x3000, 0, 0x100, PF_R); f.fail_at = 1;   CHECK(exec_load(&t, file, sizeof file, &ops) == EXEC_ERR_NOMEM); fake_free(&f); f.fail_at = -1;
    /* общая страница у двух сегментов: код [0x400000,0x400800) и данные [0x400800,0x400900): одна страница, записываемая */
    t.nseg = 2; SEG(0, 0x400000, 0x800, 0x0, 0x800, PF_R | PF_X); SEG(1, 0x400800, 0x100, 0x800, 0x80, PF_R | PF_W);
    CHECK(exec_load(&t, file, sizeof file, &ops) == EXEC_OK && f.n == 1 && f.r[0].w == 1);
    CHECK(memcmp(f.r[0].mem, file, 0x800) == 0 && memcmp(f.r[0].mem + 0x800, file + 0x800, 0x80) == 0);
    { int z = 1; for (int i = 0x880; i < 0x900; i++) if (f.r[0].mem[i]) z = 0; CHECK(z); }     /* bss сегмента данных — нули */
    fake_free(&f);
    /* сегмент, начинающийся не с границы страницы и пересекающий несколько */
    t.nseg = 1; SEG(0, 0x400F00, 0x300, 0x10, 0x300, PF_R);
    CHECK(exec_load(&t, file, sizeof file, &ops) == EXEC_OK && f.n == 2);
    CHECK(f.r[0].mem[0xF00] == file[0x10] && f.r[1].mem[0x1FF] == file[0x10 + 0x2FF] && f.r[0].mem[0xEFF] == 0 && f.r[1].mem[0x200] == 0);
    fake_free(&f);
    /* сегмент нулевого размера не создаёт страниц и не ломает остальное */
    SEG(0, 0x400000, 0, 0, 0, PF_R); CHECK(exec_load(&t, file, sizeof file, &ops) == EXEC_OK && f.n == 0);

    /* ---- стек: разбираем построенное так, как это делает _start ---- */
    {
        uint8_t *pg = aligned_alloc(4096, 4096); const uint64_t top = 0x7FFFFFFFF000ULL; uint64_t rsp;
        const char *av[] = { "hello.elf", "LimonOS", "", "x y z" };
        memset(pg, 0xEE, 4096);
        CHECK(exec_build_stack(pg, top, 4, av, &rsp) == EXEC_OK);
        CHECK((rsp & 15) == 0 && rsp >= top - 4096 && rsp < top);
        uint64_t *w = (uint64_t *)(pg + (rsp - (top - 4096)));
        CHECK(w[0] == 4);
        for (int i = 0; i < 4; i++) {
            uint64_t p = w[1 + i];
            CHECK(p >= top - 4096 && p < top && strcmp((char *)pg + (p - (top - 4096)), av[i]) == 0);
        }
        CHECK(w[5] == 0 && w[6] == 0 && w[7] == 0 && w[8] == 0);        /* NULL argv, NULL envp, auxv AT_NULL */
        CHECK((uint8_t *)&w[9] <= pg + 4096);
        /* argc = 0 */
        CHECK(exec_build_stack(pg, top, 0, av, &rsp) == EXEC_OK && (rsp & 15) == 0);
        w = (uint64_t *)(pg + (rsp - (top - 4096))); CHECK(w[0] == 0 && w[1] == 0 && w[2] == 0);
        /* слишком много/слишком длинно */
        CHECK(exec_build_stack(pg, top, EXEC_MAX_ARGS + 1, av, &rsp) == EXEC_ERR_ARGS);
        CHECK(exec_build_stack(pg, top, -1, av, &rsp) == EXEC_ERR_ARGS);
        char *big = malloc(5000); memset(big, 'a', 4999); big[4999] = 0; const char *bv[] = { big };
        CHECK(exec_build_stack(pg, top, 1, bv, &rsp) == EXEC_ERR_ARGS);
        big[3900] = 0; CHECK(exec_build_stack(pg, top, 1, bv, &rsp) == EXEC_OK);      /* почти полная страница — ещё влезает */
        big[3900] = 'a'; big[4060] = 0; CHECK(exec_build_stack(pg, top, 1, bv, &rsp) == EXEC_ERR_ARGS);   /* а теперь строка есть, а места под указатели нет */
        free(big); free(pg);
    }

    /* ---- сквозной запуск: образ hello_host.elf, загруженный НАШИМ загрузчиком ---- */
    {
        size_t hl; uint8_t *he = slurp(argv[2], &hl); struct elf64_info hi;
        CHECK(elf64_parse(he, hl, &hi) == ELF_OK);
        struct exec_ops mops = { 0, mmap_page };
        int pfd[2]; CHECK(pipe(pfd) == 0);
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            close(pfd[0]); dup2(pfd[1], 1);
            if (exec_load(&hi, he, hl, &mops) != EXEC_OK) _exit(101);
            const uint64_t top = 0x20000000ULL;                                 /* страницы стека: 8 штук под top */
            uint8_t *top_page = 0;
            for (int i = 0; i < EXEC_STACK_PAGES; i++) {
                uint64_t va = top - (uint64_t)(EXEC_STACK_PAGES - i) * 4096;
                uint8_t *p = mmap_page(0, va, 1); if (!p) _exit(102);
                if (i == EXEC_STACK_PAGES - 1) top_page = p;
            }
            const char *av[] = { "hello.elf", "LimonOS" }; uint64_t rsp;
            if (exec_build_stack(top_page, top, 2, av, &rsp) != EXEC_OK) _exit(103);
            __asm__ volatile ("mov %0, %%rsp\n jmp *%1" :: "r"(rsp), "r"(hi.entry) : "memory");
            _exit(104);
        }
        close(pfd[1]);
        char out[4096]; size_t n = 0; ssize_t r;
        while ((r = read(pfd[0], out + n, sizeof out - 1 - n)) > 0) n += (size_t)r;
        out[n] = 0;
        int st = 0; waitpid(pid, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        CHECK(strcmp(out, EXEC_HELLO_EXPECT) == 0);
        if (strcmp(out, EXEC_HELLO_EXPECT) != 0) printf("получено: [%s]\nждали:    [%s]\n", out, EXEC_HELLO_EXPECT);
        free(he);
    }

    free(elf);
    if (fails) { printf("ПРОВАЛЕНО проверок: %d\n", fails); return 1; }
    printf("exec host: все проверки прошли\n");
    return 0;
}
