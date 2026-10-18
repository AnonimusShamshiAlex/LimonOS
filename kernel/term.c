#include "term.h"
#include "format.h"
#include "kstring.h"

void term_clear(struct term *t) {
    for (int r = 0; r < TERM_ROWS; r++)
        for (int c = 0; c < TERM_COLS; c++)
            t->cell[r][c] = (struct tcell){ ' ', TC_DEFAULT };
    t->cx = t->cy = 0;
}

void term_init(struct term *t) {
    t->fg = TC_DEFAULT;
    t->utf_left = 0;
    term_clear(t);
}

void term_set_fg(struct term *t, uint32_t fg) { t->fg = fg; }

static void scroll(struct term *t) {
    memmove(&t->cell[0][0], &t->cell[1][0], sizeof(t->cell[0]) * (TERM_ROWS - 1));
    for (int c = 0; c < TERM_COLS; c++)
        t->cell[TERM_ROWS - 1][c] = (struct tcell){ ' ', TC_DEFAULT };
}

static void newline(struct term *t) {
    t->cx = 0;
    if (++t->cy >= TERM_ROWS) { scroll(t); t->cy = TERM_ROWS - 1; }
}

static void put_cp(struct term *t, uint32_t cp) {
    switch (cp) {
    case '\n': newline(t); return;
    case '\r': t->cx = 0; return;
    case '\t': do put_cp(t, ' '); while (t->cx % 4); return;
    case '\b': if (t->cx > 0) t->cx--; return;
    default: break;
    }
    if (t->cx >= TERM_COLS) newline(t);
    t->cell[t->cy][t->cx++] = (struct tcell){ cp, t->fg };
}

void term_putc(struct term *t, char ch) {
    uint8_t b = (uint8_t)ch;
    if (t->utf_left > 0) {
        if ((b & 0xC0) == 0x80) {
            t->utf_cp = (t->utf_cp << 6) | (b & 0x3F);
            if (--t->utf_left == 0) put_cp(t, t->utf_cp);
            return;
        }
        t->utf_left = 0;
    }
    if (b < 0x80) put_cp(t, b);
    else if ((b & 0xE0) == 0xC0) { t->utf_cp = b & 0x1F; t->utf_left = 1; }
    else if ((b & 0xF0) == 0xE0) { t->utf_cp = b & 0x0F; t->utf_left = 2; }
    else if ((b & 0xF8) == 0xF0) { t->utf_cp = b & 0x07; t->utf_left = 3; }
}

void term_write(struct term *t, const char *s) { while (*s) term_putc(t, *s++); }

static void tput(char c, void *ctx) { term_putc(ctx, c); }

void term_printf(struct term *t, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvformat(tput, t, fmt, ap);
    va_end(ap);
}
