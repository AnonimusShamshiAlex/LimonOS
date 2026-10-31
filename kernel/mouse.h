#ifndef LIMON_MOUSE_H
#define LIMON_MOUSE_H
#include <stdint.h>

struct mouse_event {
    int x, y;          /* позиция курсора на экране */
    uint8_t buttons;   /* бит0 — левая, бит1 — правая, бит2 — средняя */
};

void mouse_set_bounds(int w, int h);
void mouse_irq(void);                       /* вызывается из прерывания IRQ12 */
void mouse_feed_byte(uint8_t b);            /* разбор байта PS/2-пакета */
int  mouse_poll(struct mouse_event *ev);

#endif
