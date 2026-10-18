#include "power.h"
#include "io.h"

void power_reboot(void) {
    cpu_cli();
    for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 2)) break;
    outb(0x64, 0xFE);                         /* сброс через контроллер клавиатуры */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" :: "m"(null_idt));   /* тройная ошибка = перезагрузка */
    for (;;) cpu_halt();
}

void power_shutdown(void) {
    outw(0x604, 0x2000);     /* QEMU (новые версии) */
    outw(0xB004, 0x2000);    /* Bochs / старый QEMU */
    outw(0x4004, 0x3400);    /* VirtualBox */
}
