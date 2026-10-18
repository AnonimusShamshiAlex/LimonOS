/* Драйвер Intel e1000 (82540EM и родственные): работа без прерываний, опрос очереди. */
#include "nic.h"
#include "pci.h"
#include "mem.h"
#include "kstring.h"

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL0   0x5400
#define REG_RAH0   0x5404

#define N_RX 32
#define N_TX 8
#define BUF  2048

struct rx_desc { uint64_t addr; uint16_t length, csum; uint8_t status, errors; uint16_t special; } __attribute__((packed));
struct tx_desc { uint64_t addr; uint16_t length; uint8_t cso, cmd, status, css; uint16_t special; } __attribute__((packed));

static volatile uint8_t *mmio;
static struct rx_desc *rx;
static struct tx_desc *tx;
static uint8_t *rx_buf, *tx_buf;
static int rx_cur, tx_cur;
static uint8_t mac[6];
static int ready;

static uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(mmio + r); }
static void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(mmio + r) = v; }
static void barrier(void) { __asm__ volatile ("" ::: "memory"); }

static int supported(uint16_t dev) { return dev == 0x100E || dev == 0x100F || dev == 0x1004; }

int nic_init(void) {
    const struct pci_dev *d = 0;
    for (int i = 0; i < pci_count(); i++) {
        const struct pci_dev *c = pci_get(i);
        if (c->vendor == 0x8086 && supported(c->device)) { d = c; break; }
    }
    if (!d) return -1;

    uint32_t cmd = pci_read32(d->bus, d->dev, d->fn, 4);
    pci_write32(d->bus, d->dev, d->fn, 4, cmd | 0x6);              /* память + управление шиной (DMA) */
    uint32_t bar0 = pci_read32(d->bus, d->dev, d->fn, 0x10);
    if (bar0 & 1) return -2;                                        /* ожидаем память, а не порты */
    mmio = (volatile uint8_t *)phys_to_virt(bar0 & ~0xFULL);

    wr(REG_IMC, 0xFFFFFFFF);
    wr(REG_CTRL, rd(REG_CTRL) | (1u << 26));                        /* сброс */
    for (int i = 0; i < 1000000 && (rd(REG_CTRL) & (1u << 26)); i++) { }
    wr(REG_IMC, 0xFFFFFFFF);
    wr(REG_CTRL, (rd(REG_CTRL) | (1u << 6) | (1u << 5)) & ~((1u << 3) | (1u << 7) | (1u << 31)));  /* SLU, ASDE */

    uint32_t ral = rd(REG_RAL0), rah = rd(REG_RAH0);
    mac[0] = (uint8_t)ral; mac[1] = (uint8_t)(ral >> 8); mac[2] = (uint8_t)(ral >> 16); mac[3] = (uint8_t)(ral >> 24);
    mac[4] = (uint8_t)rah; mac[5] = (uint8_t)(rah >> 8);
    for (int i = 0; i < 128; i++) wr(REG_MTA + i * 4, 0);

    uint64_t rx_ring = pmm_alloc(1), tx_ring = pmm_alloc(1);
    uint64_t rx_mem = pmm_alloc(N_RX * BUF / 4096), tx_mem = pmm_alloc(N_TX * BUF / 4096);
    if (!rx_ring || !tx_ring || !rx_mem || !tx_mem) return -3;
    rx = phys_to_virt(rx_ring); tx = phys_to_virt(tx_ring);
    rx_buf = phys_to_virt(rx_mem); tx_buf = phys_to_virt(tx_mem);

    for (int i = 0; i < N_RX; i++) { rx[i].addr = rx_mem + (uint64_t)i * BUF; rx[i].status = 0; }
    for (int i = 0; i < N_TX; i++) { tx[i].addr = tx_mem + (uint64_t)i * BUF; tx[i].status = 1; }

    wr(REG_RDBAL, (uint32_t)rx_ring); wr(REG_RDBAH, (uint32_t)(rx_ring >> 32));
    wr(REG_RDLEN, N_RX * sizeof(struct rx_desc));
    wr(REG_RDH, 0); wr(REG_RDT, N_RX - 1);
    wr(REG_TDBAL, (uint32_t)tx_ring); wr(REG_TDBAH, (uint32_t)(tx_ring >> 32));
    wr(REG_TDLEN, N_TX * sizeof(struct tx_desc));
    wr(REG_TDH, 0); wr(REG_TDT, 0);
    wr(REG_TIPG, 0x0060200A);
    wr(REG_TCTL, (1u << 1) | (1u << 3) | (0x0Fu << 4) | (0x40u << 12));            /* EN, PSP, CT, COLD */
    wr(REG_RCTL, (1u << 1) | (1u << 15) | (1u << 26));                              /* EN, BAM, SECRC (буфер 2048) */

    rx_cur = tx_cur = 0;
    ready = 1;
    return 0;
}

const char *nic_name(void) { return "Intel e1000"; }
const uint8_t *nic_mac(void) { return mac; }
int nic_link(void) { return ready && (rd(REG_STATUS) & 2); }

int nic_send(const void *frame, size_t len) {
    if (!ready || len > BUF) return -1;
    struct tx_desc *t = &tx[tx_cur];
    for (int i = 0; i < 1000000 && !(t->status & 1); i++) barrier();
    if (!(t->status & 1)) return -2;
    memcpy(tx_buf + (size_t)tx_cur * BUF, frame, len);
    t->length = (uint16_t)len;
    t->cmd = 0x0B;                      /* EOP | IFCS | RS */
    t->status = 0;
    barrier();
    tx_cur = (tx_cur + 1) % N_TX;
    wr(REG_TDT, (uint32_t)tx_cur);
    return 0;
}

int nic_recv(void *buf, size_t max) {
    if (!ready) return 0;
    struct rx_desc *r = &rx[rx_cur];
    barrier();
    if (!(r->status & 1)) return 0;
    size_t len = r->length;
    if (len > max) len = max;
    memcpy(buf, rx_buf + (size_t)rx_cur * BUF, len);
    r->status = 0;
    barrier();
    wr(REG_RDT, (uint32_t)rx_cur);
    rx_cur = (rx_cur + 1) % N_RX;
    return (int)len;
}
