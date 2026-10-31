#ifndef LIMON_FAT32_H
#define LIMON_FAT32_H
#include <stdint.h>
#include <stddef.h>
#include "blk.h"
/* FAT32, только чтение. Длинные имена (VFAT) понимает. Раздел ищется без разметки или в MBR (тип 0x0B/0x0C). */
typedef void (*fat_ls_cb)(const char *name, int is_dir, uint32_t size, void *ctx);
int  fat_mount(const struct blkdev *d);                       /* 0 — успех */
int  fat_list(const char *path, fat_ls_cb cb, void *ctx);     /* 0 — успех, -1 нет пути */
int  fat_read(const char *path, uint8_t **data, size_t *len, size_t max);   /* kmalloc'нутый буфер (+1 нулевой байт); 0 успех, -1 нет файла, -2 велик/нет памяти, -3 ошибка диска */
int  fat_mounted(void);
#endif
