#ifndef LIMON_TERM_H
#define LIMON_TERM_H
#include <stdint.h>

#define TERM_COLS 80
#define TERM_ROWS 24

/* Цвета текста: 0 и 1 — "цвет темы", остальное — обычный 0xRRGGBB */
#define TC_DEFAULT 0
#define TC_ACCENT  1
#define TC_GREEN   0x7BD88F
#define TC_RED     0xFF6B6B
#define TC_GRAY    0x8A94A6
#define TC_BLUE    0x6CB6FF
#define TC_ORANGE  0xFFA94D

struct tcell { uint32_t cp; uint32_t fg; };

struct term {
    struct tcell cell[TERM_ROWS][TERM_COLS];
    int cx, cy;
    uint32_t fg;
    uint32_t utf_cp;
    int utf_left;
};

void term_init(struct term *t);
void term_clear(struct term *t);
void term_set_fg(struct term *t, uint32_t fg);
void term_putc(struct term *t, char c);      /* байты UTF-8 по одному */
void term_write(struct term *t, const char *s);
void term_printf(struct term *t, const char *fmt, ...);

#endif
