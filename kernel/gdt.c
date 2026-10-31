#include <stdint.h>
#include "arch.h"
#include "ring3.h"

/* GDT: null, код/данные ядра, данные/код пользователя (ring3), TSS (2 записи).
 * Построение дескрипторов — в ring3.h (проверено на хосте, tests/gdt_host.c);
 * здесь только загрузка в процессор (lgdt/ltr), её на хосте проверить нельзя. */
static uint64_t gdt[GDT_ENTRIES] __attribute__((aligned(16)));
static struct tss64 tss __attribute__((aligned(16)));

/* Стек ring0, на который процессор переключается при прерывании/исключении из
 * ring3 (берётся из tss.rsp0). Статический: лежит в .bss ядра, т.е. в верхней
 * половине адресов, общей для всех адресных пространств (vmm_share_upper_half). */
#define RING0_STACK_SIZE 16384
static uint8_t ring0_stack[RING0_STACK_SIZE] __attribute__((aligned(16)));

struct __attribute__((packed)) gdtr { uint16_t limit; uint64_t base; };

void gdt_set_rsp0(uint64_t rsp0) { tss.rsp0 = rsp0; }
uint64_t gdt_ring0_stack_top(void) { return (uint64_t)(ring0_stack + RING0_STACK_SIZE); }

void gdt_init(void) {
    tss.rsp0 = gdt_ring0_stack_top();
    tss.iomap_base = TSS_SIZE;          /* карты ввода-вывода нет: in/out из ring3 запрещены */
    gdt_fill(gdt, (uint64_t)&tss, TSS_SIZE - 1);

    struct gdtr g = { sizeof(gdt) - 1, (uint64_t)gdt };
    __asm__ volatile (
        "lgdt %0\n"
        "pushq %1\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw %2, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        :: "m"(g), "i"(SEL_KCODE), "i"(SEL_KDATA) : "rax", "memory");

    __asm__ volatile ("ltr %0" :: "r"((uint16_t)SEL_TSS) : "memory");
}
