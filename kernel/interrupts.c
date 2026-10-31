#include "arch.h"
#include "console.h"
#include "gfx.h"
#include "io.h"
#include "keyboard.h"
#include "mouse.h"
#include "timer.h"
#include "ring3.h"
#include "user.h"
#include "sched.h"

static const char *exc_names[32] = {
    "деление на ноль", "отладка", "NMI", "точка останова", "переполнение", "выход за границы",
    "неверная инструкция", "нет сопроцессора", "двойная ошибка", "сегмент сопроцессора",
    "неверный TSS", "сегмент отсутствует", "ошибка стека", "общая защита (GPF)",
    "ошибка страницы (page fault)", "резерв", "ошибка x87", "выравнивание", "машинная проверка",
    "ошибка SIMD", "виртуализация", "резерв", "резерв", "резерв", "резерв", "резерв",
    "резерв", "резерв", "резерв", "резерв", "безопасность", "резерв"
};

static void panic(struct regs *r) {
    cpu_cli();
    gfx_use_framebuffer();
    console_init(0xFFFFFF, 0x4A0F0F);
    kprintf("*** ПАНИКА ЯДРА LimonOS ***\n\n");
    kprintf("Исключение %d: %s\n", (int)r->int_no, exc_names[r->int_no]);
    kprintf("Код ошибки: 0x%llx\n", (unsigned long long)r->err_code);
    kprintf("RIP=%p  CS=0x%llx  RFLAGS=0x%llx\n", (void *)r->rip, (unsigned long long)r->cs, (unsigned long long)r->rflags);
    kprintf("RSP=%p  RAX=%p\n", (void *)r->rsp, (void *)r->rax);
    if (r->int_no == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        kprintf("CR2 (адрес ошибки)=%p\n", (void *)cr2);
    }
    kprintf("\nСистема остановлена. Перезагрузи компьютер.\n");
    for (;;) cpu_halt();
}

void isr_handler(struct regs *r) {
    if (r->int_no == 0x80) {           /* системный вызов: не IRQ, EOI не нужен */
        if (cs_is_user(r->cs)) { if (sched_active()) sched_on_syscall(r); else user_on_syscall(r); }
        return;
    }
    if (r->int_no < 32) {
        /* Исключение в ring3 не должно убивать ядро: если идёт сеанс user_run()/user_exec(),
         * управление вернётся в ядро с описанием сбоя (user_on_exception не возвращается).
         * Если идёт очередь Фазы 7 (sched_run_queue) — упавший процесс просто снимается,
         * очередь продолжается (sched_on_exception МОЖЕТ вернуться — переключит на другой слот).
         * Исключение в ring0, а также из ring3 вне сеанса/очереди — паника как раньше. */
        if (cs_is_user(r->cs)) {
            if (sched_active()) { sched_on_exception(r); return; }
            user_on_exception(r);
        }
        panic(r);
    }
    int irq = (int)r->int_no - 32;
    switch (irq) {
    case 0:  timer_irq(); break;
    case 1:  kbd_irq(); break;
    case 12: mouse_irq(); break;
    default: break;
    }
    pic_eoi(irq);                      /* EOI строго ДО возможного ухода через user_resume */
    if (irq == 0 && cs_is_user(r->cs)) { if (sched_active()) sched_on_timer(r); else user_on_timer(r); }
}
