#ifndef LIMON_TIMER_H
#define LIMON_TIMER_H
#include <stdint.h>
#define TIMER_HZ 100
extern volatile uint64_t timer_ticks;   /* тиков с момента запуска (100 в секунду) */
void timer_init(void);
void timer_irq(void);
#endif
