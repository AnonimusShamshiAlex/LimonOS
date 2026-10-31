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

/* Формат пикселя настоящего экрана. Всё рисование внутри ядра идёт в «стандартном» 0xRRGGBB;
   если у видеокарты другой порядок цветов (RGB вместо BGR) или не 32 бита (16/24 — бывает на
   старом железе), мы рисуем в буфер в памяти, а при выводе на экран переводим цвета. */
static struct { int bpp, rs, gs, bs, rsz, gsz, bsz; long pitch_bytes; int std; uint8_t *base; } fmt;

int gfx_init(struct limine_framebuffer *fb) {
    if (fb->bpp != 32 && fb->bpp != 24 && fb->bpp != 16) return 0;
    fmt.bpp = fb->bpp; fmt.base = (uint8_t *)fb->address; fmt.pitch_bytes = (long)fb->pitch;
    fmt.rs = fb->red_mask_shift;   fmt.rsz = fb->red_mask_size;
    fmt.gs = fb->green_mask_shift; fmt.gsz = fb->green_mask_size;
    fmt.bs = fb->blue_mask_shift;  fmt.bsz = fb->blue_mask_size;
    if (fmt.rsz == 0 && fmt.gsz == 0 && fmt.bsz == 0) {          /* загрузчик не сказал — считаем обычным BGRA */
        fmt.rs = 16; fmt.gs = 8; fmt.bs = 0; fmt.rsz = fmt.gsz = fmt.bsz = 8;
    }
    fmt.std = fb->bpp == 32 && fmt.rs == 16 && fmt.gs == 8 && fmt.bs == 0 && fmt.rsz == 8 && fmt.gsz == 8 && fmt.bsz == 8;
    screen_s.px = (uint32_t *)fb->address;
    screen_s.w = (int)fb->width;
    screen_s.h = (int)fb->height;
    screen_s.stride = (int)(fb->pitch / 4);
    tgt = &screen_s;
    if (!fmt.std) gfx_use_backbuffer();                          /* рисуем в память, на экран переводим при gfx_present */
    return 1;
}

int gfx_format_standard(void) { return fmt.std; }
int gfx_bpp(void) { return fmt.bpp; }

void gfx_use_backbuffer(void) {
    int w = screen_s.w > BACK_MAXW ? BACK_MAXW : screen_s.w;
    int h = screen_s.h > BACK_MAXH ? BACK_MAXH : screen_s.h;
    back_s.px = back_mem; back_s.w = w; back_s.h = h; back_s.stride = w;
    tgt = &back_s;
}

void gfx_use_framebuffer(void) { if (fmt.std) tgt = &screen_s; }

static inline uint32_t pack_channel(uint32_t v8, int size, int shift) {
    return (size >= 8 ? (v8 << (size - 8)) : (v8 >> (8 - size))) << shift;
}

void gfx_present(void) {
    if (tgt != &back_s) return;
    if (fmt.std) {
        for (int y = 0; y < back_s.h; y++)
            copy32(screen_s.px + (long)y * screen_s.stride, back_s.px + (long)y * back_s.stride, back_s.w);
        return;
    }
    for (int y = 0; y < back_s.h; y++) {
        const uint32_t *src = back_s.px + (long)y * back_s.stride;
        uint8_t *dst = fmt.base + (long)y * fmt.pitch_bytes;
        for (int x = 0; x < back_s.w; x++) {
            uint32_t c = src[x];
            uint32_t v = pack_channel((c >> 16) & 255, fmt.rsz, fmt.rs) | pack_channel((c >> 8) & 255, fmt.gsz, fmt.gs)
                       | pack_channel(c & 255, fmt.bsz, fmt.bs);
            if (fmt.bpp == 32) ((uint32_t *)dst)[x] = v;
            else if (fmt.bpp == 24) { dst[x * 3] = (uint8_t)v; dst[x * 3 + 1] = (uint8_t)(v >> 8); dst[x * 3 + 2] = (uint8_t)(v >> 16); }
            else ((uint16_t *)dst)[x] = (uint16_t)v;
        }
    }
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

/* Сохранить/вернуть всю текущую поверхность (кэш обоев: рисуем их один раз, дальше — быстрое копирование). */
size_t gfx_surface_pixels(void) { return (size_t)tgt->w * (size_t)tgt->h; }
void gfx_save_surface(uint32_t *dst) {
    for (int y = 0; y < tgt->h; y++) copy32(dst + (long)y * tgt->w, tgt->px + (long)y * tgt->stride, tgt->w);
}
void gfx_load_surface(const uint32_t *src) {
    for (int y = 0; y < tgt->h; y++) copy32(tgt->px + (long)y * tgt->stride, src + (long)y * tgt->w, tgt->w);
}

/* Закрашенный отрезок под наклоном (для полос и молний): рисует по строкам, толщина th. */
void gfx_fill_parallelogram(int x, int y, int w, int h, int skew, uint32_t color) {
    for (int j = 0; j < h; j++) gfx_fill_rect(x + (skew * j) / (h > 1 ? h - 1 : 1), y + j, w, 1, color);
}
