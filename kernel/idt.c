#include "arch.h"

struct __attribute__((packed)) idt_entry {
    uint16_t off_lo, sel;
    uint8_t  ist, attr;
    uint16_t off_mid;
    uint32_t off_hi, zero;
};
struct __attribute__((packed)) idtr { uint16_t limit; uint64_t base; };

static struct idt_entry idt[256];
extern uint64_t isr_table[48];

void idt_init(void) {
    for (int i = 0; i < 48; i++) {
        uint64_t a = isr_table[i];
        idt[i].off_lo = (uint16_t)a;
        idt[i].sel = 0x08;
        idt[i].ist = 0;
        idt[i].attr = 0x8E;                 /* present, прерывающий шлюз */
        idt[i].off_mid = (uint16_t)(a >> 16);
        idt[i].off_hi = (uint32_t)(a >> 32);
        idt[i].zero = 0;
    }
    struct idtr r = { sizeof(idt) - 1, (uint64_t)idt };
    __asm__ volatile ("lidt %0" :: "m"(r));
}
