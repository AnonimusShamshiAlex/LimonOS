#ifndef LIMON_BLK_H
#define LIMON_BLK_H
#include <stdint.h>
#include <stddef.h>
/* Блочное устройство: читает секторы по 512 байт. Сейчас есть драйвер ATA (IDE и SATA в режиме IDE/Legacy). */
struct blkdev {
    int (*read)(void *ctx, uint64_t lba, uint32_t count, void *buf);   /* 0 — успех */
    void *ctx;
    uint64_t sectors;
    char name[24];
};
int ata_init(void);                          /* сколько дисков найдено */
const struct blkdev *ata_disk(int i);        /* NULL, если нет */
#endif
