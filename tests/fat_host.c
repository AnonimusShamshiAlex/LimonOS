#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fat32.h"
static FILE *img;
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
static int rd(void *c, uint64_t lba, uint32_t n, void *b) { (void)c; fseek(img, (long)(lba * 512), SEEK_SET); return fread(b, 512, n, img) == n ? 0 : -1; }
static void ls(const char *n, int d, uint32_t s, void *c) { (void)c; printf("  %s%s %u\n", n, d ? "/" : "", s); }
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); fails++; } } while (0)
int main(int argc, char **argv) { (void)argc;
    int fails = 0;
    img = fopen(argv[1], "rb");
    struct blkdev d = { rd, NULL, 72000, "t" };
    CHECK(fat_mount(&d) == 0);
    puts("/:"); CHECK(fat_list("/", ls, NULL) == 0);
    puts("/long folder name:"); CHECK(fat_list("/long folder name", ls, NULL) == 0);
    uint8_t *p; size_t n;
    CHECK(fat_read("KEY.TXT", &p, &n, 1 << 20) == 0 && n == 31 && memcmp(p, "sk-or-v1-", 9) == 0);
    CHECK(fat_read("/Long Folder Name/keys.txt", &p, &n, 1 << 20) == 0 && n == 7 && !memcmp(p, "inside\n", 7));
    CHECK(fat_read("/привет мир.txt", &p, &n, 1 << 20) == 0 && n == 13);
    CHECK(fat_read("/big.bin", &p, &n, 1 << 20) == 0 && n == 5000);
    FILE *f = fopen(argv[2], "rb"); uint8_t ref[5000]; fread(ref, 1, 5000, f); CHECK(memcmp(ref, p, 5000) == 0);
    CHECK(fat_read("/big.bin", &p, &n, 100) == -2);          /* слишком большой */
    CHECK(fat_read("/nope.txt", &p, &n, 100) == -1);
    CHECK(fat_read("/long folder name", &p, &n, 100) == -1);  /* каталог — не файл */
    CHECK(fat_read("/key.txt/x", &p, &n, 100) == -1);
    printf(fails ? "FAILED %d\n" : "FAT OK\n", fails);
    return fails != 0;
}
