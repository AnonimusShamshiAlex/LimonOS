#include "arch.h"
#include "io.h"

void pic_init(void) {
    outb(0x20, 0x11); outb(0xA0, 0x11);     /* начать инициализацию */
    outb(0x21, 0x20); outb(0xA1, 0x28);     /* IRQ0-7 -> векторы 32-39, IRQ8-15 -> 40-47 */
    outb(0x21, 0x04); outb(0xA1, 0x02);     /* каскад через IRQ2 */
    outb(0x21, 0x01); outb(0xA1, 0x01);     /* режим 8086 */
    outb(0x21, (uint8_t)~((1 << 0) | (1 << 1) | (1 << 2)));   /* включены: таймер, клавиатура, каскад */
    outb(0xA1, (uint8_t)~(1 << 4));                           /* включена: мышь (IRQ12) */
}

void pic_eoi(int irq) {
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}
