#include "pci.h"
#include "io.h"

#define MAX_PCI 64
static struct pci_dev devs[MAX_PCI];
static int ndevs;

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

void pci_init(void) {
    ndevs = 0;
    for (int bus = 0; bus < 32; bus++)
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id0 = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            uint32_t hdr = pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x0C);
            int nfn = (hdr >> 16) & 0x80 ? 8 : 1;
            for (int fn = 0; fn < nfn; fn++) {
                uint32_t id = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0);
                if ((id & 0xFFFF) == 0xFFFF || ndevs >= MAX_PCI) continue;
                uint32_t cl = pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 8);
                struct pci_dev *d = &devs[ndevs++];
                d->bus = (uint8_t)bus; d->dev = (uint8_t)dev; d->fn = (uint8_t)fn;
                d->vendor = (uint16_t)(id & 0xFFFF); d->device = (uint16_t)(id >> 16);
                d->cls = (uint8_t)(cl >> 24); d->sub = (uint8_t)(cl >> 16); d->prog = (uint8_t)(cl >> 8);
            }
        }
}

int pci_count(void) { return ndevs; }
const struct pci_dev *pci_get(int i) { return (i >= 0 && i < ndevs) ? &devs[i] : 0; }

static const struct { uint16_t v, d; const char *name; } known[] = {
    { 0x8086, 0x1237, "Intel 440FX: хост-мост" },
    { 0x8086, 0x7000, "Intel PIIX3: ISA-мост" },
    { 0x8086, 0x7010, "Intel PIIX3: IDE" },
    { 0x8086, 0x7113, "Intel PIIX4: ACPI" },
    { 0x8086, 0x100E, "Intel PRO/1000 MT (e1000)" },
    { 0x8086, 0x100F, "Intel PRO/1000 MT Server" },
    { 0x8086, 0x1004, "Intel PRO/1000 T Server" },
    { 0x8086, 0x2829, "Intel ICH8: SATA AHCI" },
    { 0x8086, 0x2415, "Intel AC'97: звук" },
    { 0x8086, 0x2668, "Intel HD Audio" },
    { 0x80EE, 0xBEEF, "VirtualBox: видеоадаптер" },
    { 0x80EE, 0xCAFE, "VirtualBox: гостевые службы" },
    { 0x106B, 0x003F, "Apple OHCI: USB" },
    { 0x1AF4, 0x1000, "virtio: сетевая карта" },
    { 0x1234, 0x1111, "QEMU: видеоадаптер" },
};

const char *pci_device_name(const struct pci_dev *d) {
    for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++)
        if (known[i].v == d->vendor && known[i].d == d->device) return known[i].name;
    switch (d->cls) {
    case 0x01: return d->sub == 0x06 ? "Диск SATA" : d->sub == 0x08 ? "Диск NVMe" : d->sub == 0x01 ? "Диск IDE" : "Контроллер дисков";
    case 0x02: return "Сетевая карта";
    case 0x03: return "Видеоадаптер";
    case 0x04: return "Звук";
    case 0x06: return d->sub == 0 ? "Хост-мост" : d->sub == 1 ? "ISA-мост" : d->sub == 4 ? "Мост PCI" : "Мост";
    case 0x0C: return d->sub == 0x03 ? "Контроллер USB" : "Последовательная шина";
    default:   return "Устройство";
    }
}
