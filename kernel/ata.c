/* ATA PIO (LBA28): первичный и вторичный канал, диски master/slave. Без прерываний, только чтение.
   Видит жёсткие диски и SATA-диски в режиме IDE/Compatible (в BIOS старых ПК это обычно так).
   НЕ видит: USB-флешки, AHCI-режим, NVMe, CD (ATAPI). Проверено только по спецификации. */
#include "blk.h"
#include "io.h"
#include "kstring.h"
#include "format.h"

struct ata_dev { uint16_t io, ctl; int slave; struct blkdev bd; int present; };
static struct ata_dev devs[4];
static int ndevs;

static inline void insw(uint16_t port, void *buf, int n) {
    __asm__ volatile ("rep insw" : "+D"(buf), "+c"(n) : "d"(port) : "memory");
}

static int wait_not_busy(uint16_t io) {
    for (int i = 0; i < 4000000; i++) { if (!(inb(io + 7) & 0x80)) return 0; }
    return -1;
}
static int wait_drq(uint16_t io) {
    for (int i = 0; i < 4000000; i++) {
        uint8_t s = inb(io + 7);
        if (s & 0x01 || s & 0x20) return -1;            /* ошибка или сбой диска */
        if (!(s & 0x80) && (s & 0x08)) return 0;
    }
    return -1;
}

static int ata_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    struct ata_dev *d = ctx;
    uint8_t *out = buf;
    if (lba + count > d->bd.sectors || lba >= 0x10000000ULL) return -1;
    while (count) {
        uint32_t n = count > 128 ? 128 : count;
        if (wait_not_busy(d->io)) return -2;
        outb(d->io + 6, (uint8_t)(0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F)));
        outb(d->io + 2, (uint8_t)n);
        outb(d->io + 3, (uint8_t)lba); outb(d->io + 4, (uint8_t)(lba >> 8)); outb(d->io + 5, (uint8_t)(lba >> 16));
        outb(d->io + 7, 0x20);
        for (uint32_t s = 0; s < n; s++) {
            if (wait_drq(d->io)) return -3;
            insw(d->io, out, 256);
            out += 512;
        }
        lba += n; count -= n;
    }
    return 0;
}

static void probe(uint16_t io, uint16_t ctl, int slave) {
    if (inb(io + 7) == 0xFF) return;                    /* на шине ничего нет */
    outb(io + 6, (uint8_t)(0xA0 | (slave << 4)));
    for (int i = 0; i < 4; i++) inb(ctl);               /* 400 нс на выбор диска */
    outb(io + 2, 0); outb(io + 3, 0); outb(io + 4, 0); outb(io + 5, 0);
    outb(io + 7, 0xEC);
    if (inb(io + 7) == 0) return;
    if (wait_not_busy(io)) return;
    if (inb(io + 4) || inb(io + 5)) return;             /* ATAPI/SATA-сигнатура — не наш случай */
    if (wait_drq(io)) return;
    uint16_t id[256];
    insw(io, id, 256);
    uint32_t sec = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    if (!sec || ndevs >= 4) return;
    struct ata_dev *d = &devs[ndevs];
    d->io = io; d->ctl = ctl; d->slave = slave; d->present = 1;
    d->bd.read = ata_read; d->bd.ctx = d; d->bd.sectors = sec;
    ksnprintf(d->bd.name, sizeof d->bd.name, "hd%d", ndevs);
    ndevs++;
}

int ata_init(void) {
    ndevs = 0;
    probe(0x1F0, 0x3F6, 0); probe(0x1F0, 0x3F6, 1);
    probe(0x170, 0x376, 0); probe(0x170, 0x376, 1);
    return ndevs;
}
const struct blkdev *ata_disk(int i) { return (i >= 0 && i < ndevs) ? &devs[i].bd : NULL; }
