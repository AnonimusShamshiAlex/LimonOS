/* Проверка elf64_parse: на реальном статическом ELF64 (собранном настоящим ld,
 * путь задаётся argv[1]) и на наборе намеренно битых файлов. Без железа и без
 * эмулятора, как остальные хост-тесты в этой папке. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "elf.h"

static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %d: %s\n", __LINE__, #c); fails++; } }while(0)

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { perror("fread"); exit(2); }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s real_static_elf\n", argv[0]); return 2; }

    size_t len; uint8_t *buf = read_file(argv[1], &len);

    struct elf64_info info;
    enum elf_err e = elf64_parse(buf, len, &info);
    printf("реальный бинарь: %s (entry=0x%llx, сегментов=%d)\n", elf64_err_str(e),
           (unsigned long long)info.entry, info.nseg);
    CHECK(e == ELF_OK);
    CHECK(info.nseg >= 1);
    CHECK(info.entry >= info.lo && info.entry < info.hi);
    int any_x = 0; for (int i = 0; i < info.nseg; i++) if (info.seg[i].flags & PF_X) any_x = 1;
    CHECK(any_x);

    /* --- битые варианты, каждый должен быть ОТКЛОНЁН, а не "как-нибудь загружен" --- */

    /* 1. пустой/слишком короткий файл */
    { struct elf64_info o; CHECK(elf64_parse(buf, 4, &o) == ELF_ERR_TOO_SHORT); }

    /* 2. испорченная сигнатура */
    { uint8_t *c = malloc(len); memcpy(c, buf, len); c[0] = 0; 
      struct elf64_info o; CHECK(elf64_parse(c, len, &o) == ELF_ERR_MAGIC); free(c); }

    /* 3. подменённый класс (32-бит вместо 64) */
    { uint8_t *c = malloc(len); memcpy(c, buf, len); c[4] = 1;
      struct elf64_info o; CHECK(elf64_parse(c, len, &o) == ELF_ERR_CLASS); free(c); }

    /* 4. подменённая архитектура */
    { uint8_t *c = malloc(len); memcpy(c, buf, len); c[18] = 3; c[19] = 0; /* EM_386 */
      struct elf64_info o; CHECK(elf64_parse(c, len, &o) == ELF_ERR_MACHINE); free(c); }

    /* 5. e_type = ET_DYN (2 -> 3) — PIE мы в Фазе 1 честно не поддерживаем */
    { uint8_t *c = malloc(len); memcpy(c, buf, len); c[16] = 3; c[17] = 0;
      struct elf64_info o; CHECK(elf64_parse(c, len, &o) == ELF_ERR_TYPE); free(c); }

    /* 6. обрезанный файл посреди program header'ов */
    { size_t half = len / 2;
      struct elf64_info o; enum elf_err r = elf64_parse(buf, half, &o);
      CHECK(r == ELF_ERR_PHOFF || r == ELF_ERR_SEGMENT || r == ELF_ERR_TOO_SHORT); }

    printf(fails ? "FAILED %d\n" : "ELF OK\n", fails);
    return fails ? 1 : 0;
}
