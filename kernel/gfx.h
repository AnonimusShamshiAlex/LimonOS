#ifndef GFX_H
#define GFX_H
#include <stdint.h>
#include <stddef.h>
#include "limine.h"

/* Рисование идёт в "текущую поверхность": либо прямо в экран (framebuffer),
 * либо в буфер в памяти (двойная буферизация), который потом копируется на экран. */

int      gfx_init(struct limine_framebuffer *fb);   /* рисуем прямо в экран */
void     gfx_use_backbuffer(void);                  /* рисуем в буфер в памяти */
void     gfx_use_framebuffer(void);
void     gfx_present(void);                         /* буфер -> экран (с переводом цветов, если формат экрана нестандартный) */
int      gfx_format_standard(void);                 /* 1 — экран 32 бита BGRA, ничего переводить не нужно */
int      gfx_bpp(void);                         /* буфер -> экран */
uint32_t gfx_width(void);
uint32_t gfx_height(void);

uint32_t gfx_mix(uint32_t a, uint32_t b, uint32_t t);   /* t: 0..255 */
void gfx_put_pixel(int x, int y, uint32_t color);
void gfx_clear(uint32_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint32_t color);
void gfx_blend_rect(int x, int y, int w, int h, uint32_t color, uint32_t alpha);
void gfx_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gfx_fill_ellipse(int cx, int cy, int rx, int ry, uint32_t color);
void gfx_fill_ellipse_a(int cx, int cy, int rx, int ry, uint32_t color, uint32_t alpha);
size_t   gfx_surface_pixels(void);
void     gfx_save_surface(uint32_t *dst);          /* dst: gfx_surface_pixels() слов */
void     gfx_load_surface(const uint32_t *src);
void     gfx_fill_parallelogram(int x, int y, int w, int h, int skew, uint32_t color);
void gfx_scroll_up(int px, uint32_t color);

void gfx_draw_glyph(int x, int y, uint32_t cp, uint32_t fg, int scale);
int  gfx_draw_text(int x, int y, const char *utf8, uint32_t fg, int scale);  /* возвращает x конца */
int  gfx_text_width(const char *utf8, int scale);

#endif
