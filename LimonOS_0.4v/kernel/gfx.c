#include "gfx.h"
#include "font.h"
#include "kstring.h"

#define BACK_MAXW 1920
#define BACK_MAXH 1080

struct surf { uint32_t *px; int w, h, stride; };

static struct surf screen_s, back_s;
static struct surf *tgt = &screen_s;
static uint32_t back_mem[BACK_MAXW * BACK_MAXH];

static inline void copy32(uint32_t *d, const uint32_t *s, int n) {
    unsigned long c = (unsigned long)n;
    __asm__ volatile ("rep movsl" : "+D"(d), "+S"(s), "+c"(c) :: "memory");
}
static inline void fill32(uint32_t *d, uint32_t v, int n) {
    unsigned long c = (unsigned long)n;
    __asm__ volatile ("rep stosl" : "+D"(d), "+c"(c) : "a"(v) : "memory");
}

int gfx_init(struct limine_framebuffer *fb) {
    if (fb->bpp != 32) return 0;
    screen_s.px = (uint32_t *)fb->address;
    screen_s.w = (int)fb->width;
    screen_s.h = (int)fb->height;
    screen_s.stride = (int)(fb->pitch / 4);
    tgt = &screen_s;
    return 1;
}

void gfx_use_backbuffer(void) {
    int w = screen_s.w > BACK_MAXW ? BACK_MAXW : screen_s.w;
    int h = screen_s.h > BACK_MAXH ? BACK_MAXH : screen_s.h;
    back_s.px = back_mem; back_s.w = w; back_s.h = h; back_s.stride = w;
    tgt = &back_s;
}

void gfx_use_framebuffer(void) { tgt = &screen_s; }

void gfx_present(void) {
    if (tgt != &back_s) return;
    for (int y = 0; y < back_s.h; y++)
        copy32(screen_s.px + (long)y * screen_s.stride, back_s.px + (long)y * back_s.stride, back_s.w);
}

uint32_t gfx_width(void)  { return (uint32_t)tgt->w; }
uint32_t gfx_height(void) { return (uint32_t)tgt->h; }

uint32_t gfx_mix(uint32_t a, uint32_t b, uint32_t t) {
    uint32_t it = 255 - t;
    uint32_t r = (((a >> 16) & 255) * it + ((b >> 16) & 255) * t) / 255;
    uint32_t g = (((a >> 8) & 255) * it + ((b >> 8) & 255) * t) / 255;
    uint32_t bl = ((a & 255) * it + (b & 255) * t) / 255;
    return (r << 16) | (g << 8) | bl;
}

void gfx_put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x >= tgt->w || y >= tgt->h) return;
    tgt->px[(long)y * tgt->stride + x] = color;
}

void gfx_fill_rect(int x, int y, int w, int h, uint32_t color) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > tgt->w ? tgt->w : x + w;
    int y1 = y + h > tgt->h ? tgt->h : y + h;
    if (x1 <= x0) return;
    for (int yy = y0; yy < y1; yy++)
        fill32(tgt->px + (long)yy * tgt->stride + x0, color, x1 - x0);
}

void gfx_clear(uint32_t color) { gfx_fill_rect(0, 0, tgt->w, tgt->h, color); }

void gfx_draw_rect(int x, int y, int w, int h, uint32_t color) {
    gfx_fill_rect(x, y, w, 1, color);
    gfx_fill_rect(x, y + h - 1, w, 1, color);
    gfx_fill_rect(x, y, 1, h, color);
    gfx_fill_rect(x + w - 1, y, 1, h, color);
}

void gfx_blend_rect(int x, int y, int w, int h, uint32_t color, uint32_t alpha) {
    if (alpha >= 255) { gfx_fill_rect(x, y, w, h, color); return; }
    if (alpha == 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > tgt->w ? tgt->w : x + w;
    int y1 = y + h > tgt->h ? tgt->h : y + h;
    uint32_t sr = (color >> 16) & 255, sg = (color >> 8) & 255, sb = color & 255, ia = 255 - alpha;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = tgt->px + (long)yy * tgt->stride;
        for (int xx = x0; xx < x1; xx++) {
            uint32_t d = row[xx];
            uint32_t r = (((d >> 16) & 255) * ia + sr * alpha) / 255;
            uint32_t g = (((d >> 8) & 255) * ia + sg * alpha) / 255;
            uint32_t b = ((d & 255) * ia + sb * alpha) / 255;
            row[xx] = (r << 16) | (g << 8) | b;
        }
    }
}

void gfx_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    for (int j = 0; j < h; j++) {
        uint32_t t = h > 1 ? (uint32_t)(j * 255 / (h - 1)) : 0;
        gfx_fill_rect(x, y + j, w, 1, gfx_mix(top, bottom, t));
    }
}

static uint64_t isqrt64(uint64_t n) {
    uint64_t x = 0, bit = 1ULL << 62;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= x + bit) { n -= x + bit; x = (x >> 1) + bit; }
        else x >>= 1;
        bit >>= 2;
    }
    return x;
}

void gfx_fill_ellipse_a(int cx, int cy, int rx, int ry, uint32_t color, uint32_t alpha) {
    if (rx <= 0 || ry <= 0) return;
    int64_t rx2 = (int64_t)rx * rx, ry2 = (int64_t)ry * ry;
    for (int dy = -ry; dy <= ry; dy++) {
        uint64_t t = (uint64_t)(rx2 * (ry2 - (int64_t)dy * dy) / ry2);
        int half = (int)isqrt64(t);
        gfx_blend_rect(cx - half, cy + dy, half * 2 + 1, 1, color, alpha);
    }
}

void gfx_fill_ellipse(int cx, int cy, int rx, int ry, uint32_t color) {
    gfx_fill_ellipse_a(cx, cy, rx, ry, color, 255);
}

void gfx_scroll_up(int px, uint32_t color) {
    if (px <= 0) return;
    if (px >= tgt->h) { gfx_clear(color); return; }
    for (int y = 0; y < tgt->h - px; y++)
        copy32(tgt->px + (long)y * tgt->stride, tgt->px + (long)(y + px) * tgt->stride, tgt->w);
    gfx_fill_rect(0, tgt->h - px, tgt->w, px, color);
}

void gfx_draw_glyph(int x, int y, uint32_t cp, uint32_t fg, int scale) {
    const uint8_t *g = font_glyph(cp);
    for (int j = 0; j < FONT_H; j++) {
        uint8_t bits = g[j];
        if (!bits) continue;
        for (int i = 0; i < FONT_W; i++) {
            if (!(bits & (0x80 >> i))) continue;
            if (scale == 1) gfx_put_pixel(x + i, y + j, fg);
            else gfx_fill_rect(x + i * scale, y + j * scale, scale, scale, fg);
        }
    }
}

int gfx_draw_text(int x, int y, const char *s, uint32_t fg, int scale) {
    while (*s) {
        uint32_t cp = utf8_next(&s);
        gfx_draw_glyph(x, y, cp, fg, scale);
        x += FONT_W * scale;
    }
    return x;
}

int gfx_text_width(const char *s, int scale) { return utf8_len(s) * FONT_W * scale; }
