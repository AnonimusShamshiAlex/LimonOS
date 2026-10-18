#ifndef LIMON_ARCH_H
#define LIMON_ARCH_H
#include <stdint.h>

/* Состояние регистров, которое сохраняет isr_common (см. isr.S) */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

void gdt_init(void);
void idt_init(void);
void pic_init(void);
void pic_eoi(int irq);
void isr_handler(struct regs *r);

#endif
