/* Простая консоль для ранней загрузки и экрана паники (рисует прямо на экран). */
#include <stdarg.h>
#include <stddef.h>
#include "console.h"
#include "gfx.h"
#include "font.h"
#include "format.h"

static int cur_x, cur_y, cols, rows, scale = 1;
static uint32_t col_fg, col_bg;
static uint32_t utf_cp;
static int utf_left;

void console_set_color(uint32_t fg, uint32_t bg) { col_fg = fg; col_bg = bg; }

void console_clear(void) {
    gfx_clear(col_bg);
    cur_x = cur_y = 0;
}

void console_init(uint32_t fg, uint32_t bg) {
    col_fg = fg; col_bg = bg;
    int w = (int)gfx_width(), h = (int)gfx_height();
    scale = (h >= 1000) ? 2 : 1;
    cols = w / (FONT_W * scale);
    rows = h / (FONT_H * scale);
    utf_left = 0;
    console_clear();
}

static void draw_cell(uint32_t cp, int cx, int cy) {
    int px = cx * FONT_W * scale, py = cy * FONT_H * scale;
    gfx_fill_rect(px, py, FONT_W * scale, FONT_H * scale, col_bg);
    gfx_draw_glyph(px, py, cp, col_fg, scale);
}

static void newline(void) {
    cur_x = 0;
    if (++cur_y >= rows) {
        gfx_scroll_up(FONT_H * scale, col_bg);
        cur_y = rows - 1;
    }
}

static void put_codepoint(uint32_t cp) {
    if (cp == '\n') { newline(); return; }
    if (cp == '\r') { cur_x = 0; return; }
    if (cp == '\t') { do put_codepoint(' '); while (cur_x % 4); return; }
    if (cur_x >= cols) newline();
    draw_cell(cp, cur_x, cur_y);
    cur_x++;
}

void console_putc(char ch) {
    uint8_t b = (uint8_t)ch;
    if (utf_left > 0) {
        if ((b & 0xC0) == 0x80) {
            utf_cp = (utf_cp << 6) | (b & 0x3F);
            if (--utf_left == 0) put_codepoint(utf_cp);
            return;
        }
        utf_left = 0;
    }
    if (b < 0x80) put_codepoint(b);
    else if ((b & 0xE0) == 0xC0) { utf_cp = b & 0x1F; utf_left = 1; }
    else if ((b & 0xF0) == 0xE0) { utf_cp = b & 0x0F; utf_left = 2; }
    else if ((b & 0xF8) == 0xF0) { utf_cp = b & 0x07; utf_left = 3; }
}

void console_write(const char *s) { while (*s) console_putc(*s++); }

static void con_put(char c, void *ctx) { (void)ctx; console_putc(c); }

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvformat(con_put, NULL, fmt, ap);
    va_end(ap);
}
