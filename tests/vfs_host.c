#include <stdio.h>
#include <string.h>
#include "vfs.h"
#include <stdlib.h>
void *kmalloc(size_t n){return malloc(n?n:1);}
void kfree(void *p){free(p);}
#define CHECK(c) do{ if(!(c)){ printf("FAIL %d: %s\n", __LINE__, #c); fails++; } }while(0)
static int fails;
static void ls_print(int idx, const char *name, int dir, size_t sz, void *ctx) { (void)idx; (void)ctx; printf("  %s%s %zu\n", name, dir ? "/" : "", sz); }
static int count_ctx;
static void count_cb(int i,const char*n,int d,size_t z,void*c){(void)i;(void)n;(void)d;(void)z;(void)c;count_ctx++;}
int main(void) {
    vfs_init();
    count_ctx = 0; vfs_list(VFS_ROOT, count_cb, NULL);
    CHECK(count_ctx == 5);   /* ровно 5 системных папок, корень не должен быть сам себе ребёнком */
    CHECK(vfs_desktop >= 0 && vfs_downloads >= 0 && vfs_is_dir(vfs_downloads));
    CHECK(vfs_resolve(VFS_ROOT, "/Downloads") == vfs_downloads);
    CHECK(vfs_resolve(vfs_downloads, "..") == VFS_ROOT);
    CHECK(vfs_resolve(VFS_ROOT, "/nope") == VFS_NONE);
    uint8_t *d = malloc(5); memcpy(d, "hello", 5);
    int f = vfs_put(vfs_downloads, "a.txt", d, 5);
    CHECK(f >= 0);
    CHECK(vfs_resolve(VFS_ROOT, "/Downloads/a.txt") == f);
    CHECK(vfs_len(f) == 5 && memcmp(vfs_data(f), "hello", 5) == 0);
    puts("ls /Downloads:"); vfs_list(vfs_downloads, ls_print, NULL);
    char path[64]; vfs_path(f, path, sizeof path);
    printf("path=%s\n", path); CHECK(strcmp(path, "/Downloads/a.txt") == 0);
    int sub = vfs_mkdir(vfs_desktop, "Проект");
    CHECK(sub >= 0);
    int f2 = vfs_copy(f, sub, "b.txt");
    CHECK(f2 >= 0 && vfs_len(f2) == 5);
    CHECK(vfs_rename(f2, vfs_desktop, "b.txt") == 0);
    CHECK(vfs_resolve(VFS_ROOT, "/Desktop/b.txt") == f2);
    CHECK(vfs_delete(sub) == 0);                /* теперь пустая */
    CHECK(vfs_delete(VFS_ROOT) == -1);           /* корень не удалить */
    CHECK(vfs_mkdir(vfs_downloads, "../evil") == VFS_NONE);
    CHECK(vfs_mkdir(vfs_downloads, "a/b") == VFS_NONE);
    CHECK(vfs_put(vfs_downloads, "a.txt", (uint8_t*)strdup("x"), 1) == f);   /* перезапись того же имени */
    CHECK(vfs_len(f) == 1);
    printf(fails ? "FAILED %d\n" : "VFS OK\n", fails);
    return fails != 0;
}
