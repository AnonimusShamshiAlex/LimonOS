#include <stdint.h>
#include "arch.h"

static uint64_t gdt[3] = {
    0,
    0x00AF9A000000FFFFULL,   /* 0x08: код ядра, 64 бита */
    0x00CF92000000FFFFULL,   /* 0x10: данные ядра */
};

struct __attribute__((packed)) gdtr { uint16_t limit; uint64_t base; };

void gdt_init(void) {
    struct gdtr g = { sizeof(gdt) - 1, (uint64_t)gdt };
    __asm__ volatile (
        "lgdt %0\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        :: "m"(g) : "rax", "memory");
}
