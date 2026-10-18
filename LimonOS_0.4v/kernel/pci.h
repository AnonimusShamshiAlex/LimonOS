#ifndef LIMON_PCI_H
#define LIMON_PCI_H
#include <stdint.h>

struct pci_dev {
    uint8_t bus, dev, fn;
    uint16_t vendor, device;
    uint8_t cls, sub, prog;
};

void     pci_init(void);                    /* перебирает шину */
int      pci_count(void);
const struct pci_dev *pci_get(int i);
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v);
const char *pci_device_name(const struct pci_dev *d);   /* название по идентификатору или классу */

#endif
