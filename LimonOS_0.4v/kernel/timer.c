#include "timer.h"
#include "io.h"

volatile uint64_t timer_ticks;

void timer_init(void) {
    uint32_t div = 1193182 / TIMER_HZ;
    outb(0x43, 0x36);                 /* канал 0, режим 3 */
    outb(0x40, (uint8_t)(div & 0xFF));
    outb(0x40, (uint8_t)(div >> 8));
}

void timer_irq(void) { timer_ticks++; }
