#ifndef LIMON_KEYBOARD_H
#define LIMON_KEYBOARD_H
#include <stdint.h>

/* Спецклавиши (значения выше диапазона Unicode) */
#define KEY_UP      0x110001
#define KEY_DOWN    0x110002
#define KEY_LEFT    0x110003
#define KEY_RIGHT   0x110004
#define KEY_HOME    0x110005
#define KEY_END     0x110006
#define KEY_DELETE  0x110007
#define KEY_PGUP    0x110008
#define KEY_PGDN    0x110009
#define KEY_INSERT  0x11000A
#define KEY_F1      0x110011   /* F2 = KEY_F1 + 1 и т.д. */
#define KEY_F(n)    (KEY_F1 + (n) - 1)

#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4

#define LAYOUT_EN 0
#define LAYOUT_RU 1

struct key_event {
    uint32_t cp;      /* символ Unicode или KEY_* */
    uint8_t  mods;
};

void kbd_init(void);
void kbd_irq(void);                      /* вызывается из прерывания IRQ1 */
void kbd_feed_scancode(uint8_t sc);      /* разбор скан-кода (набор 1) */
int  kbd_poll(struct key_event *ev);     /* 1 — событие получено */
int  kbd_layout(void);
void kbd_set_layout(int layout);

#endif
