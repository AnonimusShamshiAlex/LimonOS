/* Драйвер Realtek RTL8139 (порты ввода-вывода, без прерываний — опрос).
   Приём: один кольцевой буфер 8 КиБ + 16 + 1500 запаса. Передача: 4 дескриптора по кругу.
   ВНИМАНИЕ: проверено только чтением спецификации, на настоящей карте ещё не запускалось. */
#include "nic.h"
#include "nic_drv.h"
#include "pci.h"
#include "mem.h"
#include "io.h"
#include "kstring.h"

#define R_IDR0    0x00
#define R_TSD0    0x10
#define R_TSAD0   0x20
#define R_RBSTART 0x30
#define R_CMD     0x37
#define R_CAPR    0x38
#define R_IMR     0x3C
#define R_ISR     0x3E
#define R_TCR     0x40
#define R_RCR     0x44
#define R_CONFIG1 0x52
#define R_MSR     0x58      /* бит 2 (LINKB) = 1 — нет линка */

#define RX_RING   8192
#define RX_BUF    (RX_RING + 16 + 1536)
#define TX_BUF    2048

static uint16_t iob;
static uint8_t *rx_virt, *tx_virt;
static uint64_t rx_phys, tx_phys;
static uint32_t rx_off;       /* где читаем в кольце */
static int tx_cur;
static uint8_t mac[6];
static int ready;

static const uint16_t ids[] = { 0x8139, 0x8138 };   /* RTL8139, RTL8139B/C+ (общий режим) */

int rtl8139_init(void) {
    const struct pci_dev *d = 0;
    for (int i = 0; i < pci_count() && !d; i++) {
        const struct pci_dev *c = pci_get(i);
        if (c->vendor != 0x10EC) continue;
        for (unsigned k = 0; k < sizeof ids / sizeof ids[0]; k++) if (c->device == ids[k]) { d = c; break; }
    }
    if (!d) return -1;

    uint32_t cmd = pci_read32(d->bus, d->dev, d->fn, 4);
    pci_write32(d->bus, d->dev, d->fn, 4, cmd | 0x5);              /* порты ввода-вывода + управление шиной (DMA) */
    uint32_t bar0 = pci_read32(d->bus, d->dev, d->fn, 0x10);
    if (!(bar0 & 1)) return -2;                                    /* ждём порты, а не память */
    iob = (uint16_t)(bar0 & 0xFFFC);

    outb(iob + R_CONFIG1, 0x00);                                   /* включить питание карты */
    outb(iob + R_CMD, 0x10);                                       /* программный сброс */
    for (int i = 0; i < 1000000 && (inb(iob + R_CMD) & 0x10); i++) { }
    if (inb(iob + R_CMD) & 0x10) return -3;

    for (int i = 0; i < 6; i++) mac[i] = inb(iob + R_IDR0 + i);

    uint64_t rp = pmm_alloc((RX_BUF + 4095) / 4096), tp = pmm_alloc((4 * TX_BUF + 4095) / 4096);
    if (!rp || !tp || rp > 0xFFFFFFFFULL || tp > 0xFFFFFFFFULL) return -4;   /* карта умеет только 32-битные адреса */
    rx_phys = rp; tx_phys = tp;
    rx_virt = phys_to_virt(rp); tx_virt = phys_to_virt(tp);

    outl(iob + R_RBSTART, (uint32_t)rx_phys);
    outw(iob + R_IMR, 0x0000);                                     /* прерывания не нужны — опрашиваем */
    outl(iob + R_RCR, 0x0000000F | (1u << 7));                     /* AAP+APM+AM+AB (свои, широковещательные), WRAP=1 */
    outb(iob + R_CMD, 0x0C);                                       /* RE + TE */
    outl(iob + R_TCR, 0x03000700);                                 /* IFG стандартный, DMA burst 2048 */

    rx_off = 0; tx_cur = 0;
    ready = 1;
    return 0;
}

const char *rtl8139_name(void) { return "Realtek RTL8139"; }
const uint8_t *rtl8139_mac(void) { return mac; }
int rtl8139_link(void) { return ready && !(inb(iob + R_MSR) & 0x04); }

int rtl8139_send(const void *frame, size_t len) {
    if (!ready || len > 1792) return -1;
    uint16_t tsd = (uint16_t)(R_TSD0 + tx_cur * 4);
    for (int i = 0; i < 1000000; i++) {                            /* ждём, пока предыдущая передача этого слота закончится (OWN=1) */
        uint32_t st = inl(iob + tsd);
        if (st & (1u << 13)) break;
        if (i == 999999) return -2;
    }
    size_t n = len < 60 ? 60 : len;                                /* минимальный кадр Ethernet без CRC */
    uint8_t *dst = tx_virt + (size_t)tx_cur * TX_BUF;
    memcpy(dst, frame, len);
    if (n > len) memset(dst + len, 0, n - len);
    outl(iob + R_TSAD0 + tx_cur * 4, (uint32_t)(tx_phys + (uint64_t)tx_cur * TX_BUF));
    outl(iob + tsd, (uint32_t)n);                                  /* запись длины запускает передачу (OWN=0) */
    tx_cur = (tx_cur + 1) & 3;
    return 0;
}

int rtl8139_recv(void *buf, size_t max) {
    if (!ready) return 0;
    if (inb(iob + R_CMD) & 0x01) return 0;                         /* BUFE: буфер приёма пуст */
    uint8_t *p = rx_virt + rx_off;
    uint16_t status = (uint16_t)(p[0] | (p[1] << 8));
    uint16_t len = (uint16_t)(p[2] | (p[3] << 8));                 /* вместе с 4 байтами CRC */
    if (!(status & 1) || len < 8 || len > 1792 + 4) {              /* мусор: сбрасываем приёмник и начинаем заново */
        outb(iob + R_CMD, 0x04); outb(iob + R_CMD, 0x0C);
        rx_off = 0; outl(iob + R_RBSTART, (uint32_t)rx_phys); outw(iob + R_CAPR, (uint16_t)(0 - 16));
        return 0;
    }
    size_t flen = (size_t)len - 4;
    size_t copy = flen < max ? flen : max;
    memcpy(buf, rx_virt + rx_off + 4, copy);                        /* WRAP=1: кадр за концом кольца лежит подряд, склеивать не нужно */
    rx_off = (rx_off + (uint32_t)len + 4 + 3) & ~3u;               /* заголовок 4 байта + кадр, выравнивание по 4 */
    rx_off %= RX_RING;
    outw(iob + R_CAPR, (uint16_t)(rx_off - 16));
    return (int)copy;
}
