/* Рабочий стол LimonOS: окна, панель задач, меню "Лимон", значки, курсор мыши. */
#include "desktop.h"
#include "gfx.h"
#include "font.h"
#include "term.h"
#include "shell.h"
#include "timer.h"
#include "rtc.h"
#include "power.h"
#include "io.h"
#include "sysinfo.h"
#include "net.h"
#include "format.h"
#include "kstring.h"

#define TASKBAR_H 32
#define TITLE_H   24
#define PAD       4
#define MAX_WIN   8
#define MAX_TERMS 4
#define START_W   96
#define TASKBTN_W 150

enum { WIN_TERM, WIN_ABOUT };

struct theme {
    const char *name;
    uint32_t bg_top, bg_bot, accent, on_accent, title_off, taskbar, body, term_bg, term_fg;
};

static const struct theme themes[] = {
    { "lemon",  0x2B3A4A, 0x10161E, 0xF4D91A, 0x14181F, 0x3A4554, 0x151B23, 0x1B222C, 0x0F141B, 0xD8DEE9 },
    { "lime",   0x24402B, 0x0C1A10, 0x8BD13A, 0x10180A, 0x35503B, 0x0F1B13, 0x16241A, 0x0C140F, 0xDDEBDD },
    { "orange", 0x40291A, 0x180D08, 0xFF9F1C, 0x1A1008, 0x55392A, 0x1D120B, 0x2A1A10, 0x150C07, 0xF1E3D6 },
    { "berry",  0x3A2145, 0x140A1A, 0xE0568C, 0x1A0A12, 0x4E3560, 0x1A0F22, 0x251530, 0x110818, 0xEBDDF0 },
    { "night",  0x16233A, 0x04070D, 0x6CB6FF, 0x08101A, 0x2B3C57, 0x0A111C, 0x101A2B, 0x070C14, 0xD6E4F5 },
};
#define NTHEMES ((int)(sizeof themes / sizeof themes[0]))

struct window {
    int used, minimized, type;
    int x, y, w, h;
    char title[32];
    struct term *term;
    struct shell *sh;
    int slot;
};

static struct window wins[MAX_WIN];
static int zorder[MAX_WIN], zcount;
static struct term terms[MAX_TERMS];
static struct shell shells[MAX_TERMS];
static uint8_t term_used[MAX_TERMS];

static int dw, dh;                 /* размер рабочего стола */
static int cur_theme;
static int redraw = 1;
static int mx, my;
static int prev_left;
static int drag_win = -1, drag_dx, drag_dy;
static int start_open;
static uint64_t last_click_tick;
static int last_click_icon = -1;
static int blink_phase;
static struct rtc_time now;
static uint64_t last_sec = ~0ULL;

static const struct theme *T(void) { return &themes[cur_theme]; }

/* ---------- темы ---------- */

int desktop_theme_count(void) { return NTHEMES; }
int desktop_theme_index(void) { return cur_theme; }
const char *desktop_theme_name(void) { return themes[cur_theme].name; }
const char *desktop_theme_name_at(int i) { return (i >= 0 && i < NTHEMES) ? themes[i].name : "?"; }
uint32_t desktop_theme_accent(int i) { return (i >= 0 && i < NTHEMES) ? themes[i].accent : 0xFFFFFF; }
void desktop_set_theme(int i) { if (i >= 0 && i < NTHEMES) { cur_theme = i; redraw = 1; } }
int desktop_theme_find(const char *name) {
    for (int i = 0; i < NTHEMES; i++) if (strcmp(themes[i].name, name) == 0) return i;
    return -1;
}

/* ---------- окна ---------- */

static int z_find(int idx) { for (int i = 0; i < zcount; i++) if (zorder[i] == idx) return i; return -1; }

static void z_to_top(int idx) {
    int i = z_find(idx);
    if (i < 0) return;
    for (; i < zcount - 1; i++) zorder[i] = zorder[i + 1];
    zorder[zcount - 1] = idx;
}

static void z_remove(int idx) {
    int i = z_find(idx);
    if (i < 0) return;
    for (; i < zcount - 1; i++) zorder[i] = zorder[i + 1];
    zcount--;
}

static int top_window(void) {
    for (int i = zcount - 1; i >= 0; i--)
        if (!wins[zorder[i]].minimized) return zorder[i];
    return -1;
}

static void focus_window(int idx) { wins[idx].minimized = 0; z_to_top(idx); }

static int alloc_window(int type, int w, int h, const char *title) {
    int idx = -1;
    for (int i = 0; i < MAX_WIN; i++) if (!wins[i].used) { idx = i; break; }
    if (idx < 0) return -1;
    struct window *win = &wins[idx];
    memset(win, 0, sizeof *win);
    win->used = 1; win->type = type; win->w = w; win->h = h;
    int k = 0;
    for (int i = 0; i < MAX_WIN; i++) if (wins[i].used && i != idx) k++;
    win->x = 130 + (k % 6) * 28;
    win->y = 20 + (k % 6) * 28;
    if (win->x + w > dw) win->x = dw - w;
    if (win->y + h > dh - TASKBAR_H) win->y = dh - TASKBAR_H - h;
    if (win->x < 0) win->x = 0;
    if (win->y < 0) win->y = 0;
    ksnprintf(win->title, sizeof win->title, "%s", title);
    zorder[zcount++] = idx;
    return idx;
}

int desktop_open_terminal(void) {
    int slot = -1;
    for (int i = 0; i < MAX_TERMS; i++) if (!term_used[i]) { slot = i; break; }
    if (slot < 0) return -1;
    char title[32];
    ksnprintf(title, sizeof title, "Терминал %d", slot + 1);
    int wi = alloc_window(WIN_TERM, TERM_COLS * FONT_W + 2 * PAD + 2, TITLE_H + TERM_ROWS * FONT_H + 2 * PAD + 1, title);
    if (wi < 0) return -1;
    term_used[slot] = 1;
    wins[wi].term = &terms[slot];
    wins[wi].sh = &shells[slot];
    wins[wi].slot = slot;
    term_init(&terms[slot]);
    shell_init(&shells[slot], &terms[slot]);
    focus_window(wi);
    redraw = 1;
    return wi;
}

void desktop_open_about(void) {
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].used && wins[i].type == WIN_ABOUT) { focus_window(i); redraw = 1; return; }
    int wi = alloc_window(WIN_ABOUT, 470, 330, "О системе");
    if (wi >= 0) focus_window(wi);
    redraw = 1;
}

static void close_window(int idx) {
    struct window *w = &wins[idx];
    if (w->type == WIN_TERM) term_used[w->slot] = 0;
    w->used = 0;
    z_remove(idx);
    redraw = 1;
}

/* ---------- геометрия ---------- */

static int in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

static int win_at(int px, int py) {
    for (int i = zcount - 1; i >= 0; i--) {
        struct window *w = &wins[zorder[i]];
        if (!w->minimized && in_rect(px, py, w->x, w->y, w->w, w->h)) return zorder[i];
    }
    return -1;
}

static int close_hit(const struct window *w, int px, int py) { return in_rect(px, py, w->x + w->w - 22, w->y + 4, 18, 16); }
static int min_hit(const struct window *w, int px, int py)   { return in_rect(px, py, w->x + w->w - 44, w->y + 4, 18, 16); }

#define ICON_COUNT 2
static const char *const icon_label[ICON_COUNT] = { "Терминал", "О системе" };
static void icon_rect(int i, int *x, int *y) { *x = 12; *y = 16 + i * 100; }

static const char *const menu_items[] = { "Терминал", "О системе", "Следующая тема", "Перезагрузка", "Выключение" };
#define MENU_N 5
#define MENU_W 210
#define MENU_ITEM_H 28
static void menu_rect(int *x, int *y, int *w, int *h) {
    *w = MENU_W; *h = MENU_N * MENU_ITEM_H + 8; *x = 0; *y = dh - TASKBAR_H - *h;
}

/* ---------- рисование ---------- */

static void draw_lemon(int cx, int cy, int r, uint32_t alpha) {
    gfx_fill_ellipse_a(cx, cy, r + r / 3, r, 0xF4D91A, alpha);
    gfx_fill_ellipse_a(cx - r / 3, cy - r / 3, r / 3, r / 4, 0xFFF9C4, alpha / 2);
    gfx_fill_ellipse_a(cx + r + r / 3, cy - r / 8, r / 8 + 1, r / 8 + 1, 0xE0C010, alpha);
    gfx_fill_ellipse_a(cx + r / 2, cy - r - r / 6, r / 3, r / 6 + 1, 0x4CAF50, alpha);
}

static void draw_wallpaper(void) {
    const struct theme *t = T();
    gfx_vgradient(0, 0, dw, dh, t->bg_top, t->bg_bot);
    int r = dh / 7;
    draw_lemon(dw * 4 / 5, dh * 2 / 5, r, 26);
    const char *mark = "LimonOS";
    int tw = gfx_text_width(mark, 3);
    gfx_draw_text(dw - tw - 24, dh - TASKBAR_H - 64, mark, gfx_mix(t->bg_bot, t->accent, 70), 3);
}

static void draw_icons(void) {
    const struct theme *t = T();
    for (int i = 0; i < ICON_COUNT; i++) {
        int x, y;
        icon_rect(i, &x, &y);
        if (in_rect(mx, my, x, y, 88, 90)) gfx_blend_rect(x, y, 88, 90, 0xFFFFFF, 28);
        int px = x + 20, py = y + 6;
        if (i == 0) {
            gfx_fill_rect(px, py, 48, 48, 0x0F141B);
            gfx_draw_rect(px, py, 48, 48, t->accent);
            gfx_fill_rect(px, py, 48, 6, t->accent);
            gfx_draw_text(px + 8, py + 12, ">_", t->accent, 2);
        } else {
            gfx_fill_ellipse(px + 24, py + 24, 24, 24, t->accent);
            gfx_draw_text(px + 16, py + 8, "i", t->on_accent, 2);
        }
        int lw = gfx_text_width(icon_label[i], 1);
        gfx_draw_text(x + 44 - lw / 2 + 1, y + 63, icon_label[i], 0x000000, 1);
        gfx_draw_text(x + 44 - lw / 2, y + 62, icon_label[i], 0xFFFFFF, 1);
    }
}

static uint32_t term_color(uint32_t fg) {
    if (fg == TC_DEFAULT) return T()->term_fg;
    if (fg == TC_ACCENT) return T()->accent;
    return fg;
}

static void draw_term_content(struct window *w, int focused) {
    const struct theme *t = T();
    struct term *tm = w->term;
    int cx0 = w->x + 1 + PAD, cy0 = w->y + TITLE_H + PAD;
    gfx_fill_rect(cx0 - PAD, cy0 - PAD, w->w - 2, w->h - TITLE_H - 1, t->term_bg);
    for (int r = 0; r < TERM_ROWS; r++)
        for (int c = 0; c < TERM_COLS; c++) {
            struct tcell *cell = &tm->cell[r][c];
            if (cell->cp != ' ')
                gfx_draw_glyph(cx0 + c * FONT_W, cy0 + r * FONT_H, cell->cp, term_color(cell->fg), 1);
        }
    if (focused && blink_phase == 0 && tm->cx < TERM_COLS) {
        int px = cx0 + tm->cx * FONT_W, py = cy0 + tm->cy * FONT_H;
        gfx_fill_rect(px, py, FONT_W, FONT_H, t->accent);
        uint32_t cp = tm->cell[tm->cy][tm->cx].cp;
        if (cp != ' ') gfx_draw_glyph(px, py, cp, t->term_bg, 1);
    }
}

static void about_line(int x, int *y, uint32_t color, const char *fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    gfx_draw_text(x, *y, buf, color, 1);
    *y += 18;
}

static void draw_about_content(struct window *w) {
    const struct theme *t = T();
    int x0 = w->x + 1, y0 = w->y + TITLE_H;
    gfx_fill_rect(x0, y0, w->w - 2, w->h - TITLE_H - 1, t->body);
    draw_lemon(x0 + 70, y0 + 70, 38, 255);
    int tx = x0 + 150, y = y0 + 22;
    gfx_draw_text(tx, y, "LimonOS " LIMON_VERSION, t->accent, 2);
    y += 40;
    about_line(tx, &y, t->term_fg, "Собственная операционная");
    about_line(tx, &y, t->term_fg, "система, написанная с нуля.");
    y = y0 + 150;
    x0 += 20;
    char up[16];
    uint64_t sec = timer_ticks / TIMER_HZ;
    ksnprintf(up, sizeof up, "%02d:%02d:%02d", (int)(sec / 3600), (int)(sec / 60 % 60), (int)(sec % 60));
    about_line(x0, &y, t->term_fg, "Экран:     %ux%u", g_sys.fb_w, g_sys.fb_h);
    about_line(x0, &y, t->term_fg, "Процессор: %s", g_sys.cpu.brand[0] ? g_sys.cpu.brand : g_sys.cpu.vendor);
    if (g_sys.mem_valid) about_line(x0, &y, t->term_fg, "Память:    %llu МиБ свободно", (unsigned long long)(g_sys.mem_usable >> 20));
    about_line(x0, &y, t->term_fg, "Работаю:   %s", up);
    {
        const struct netinfo *ni = net_info();
        char ipb[16];
        net_ip_str(ni->ip, ipb);
        about_line(x0, &y, t->term_fg, "Сеть:      %s", ni->configured ? ipb : "нет адреса");
    }
    y += 8;
    about_line(x0, &y, t->accent, "F2 язык   F3 терминал   F4 закрыть окно");
    about_line(x0, &y, t->accent, "F5 тема   Alt+Tab переключить окно");
}

static void draw_window(int idx, int focused) {
    const struct theme *t = T();
    struct window *w = &wins[idx];
    uint32_t bar = focused ? t->accent : t->title_off;
    uint32_t bar_text = focused ? t->on_accent : 0xC8CED8;
    gfx_blend_rect(w->x + 5, w->y + 5, w->w, w->h, 0x000000, 90);
    gfx_fill_rect(w->x, w->y, w->w, w->h, bar);
    gfx_fill_rect(w->x + 1, w->y + TITLE_H, w->w - 2, w->h - TITLE_H - 1, t->body);
    gfx_draw_text(w->x + 8, w->y + 4, w->title, bar_text, 1);
    uint32_t btn = focused ? gfx_mix(t->accent, 0x000000, 45) : gfx_mix(t->title_off, 0x000000, 35);
    int closing = close_hit(w, mx, my), mining = min_hit(w, mx, my);
    gfx_fill_rect(w->x + w->w - 22, w->y + 4, 18, 16, closing ? 0xE5484D : btn);
    gfx_fill_rect(w->x + w->w - 44, w->y + 4, 18, 16, mining ? gfx_mix(btn, 0xFFFFFF, 60) : btn);
    gfx_draw_text(w->x + w->w - 21, w->y + 4, "×", 0xFFFFFF, 1);
    gfx_draw_text(w->x + w->w - 43, w->y + 2, "_", 0xFFFFFF, 1);
    if (w->type == WIN_TERM) draw_term_content(w, focused);
    else draw_about_content(w);
}

static void draw_taskbar(int focus) {
    const struct theme *t = T();
    int y = dh - TASKBAR_H;
    gfx_fill_rect(0, y, dw, TASKBAR_H, t->taskbar);
    gfx_fill_rect(0, y, dw, 1, gfx_mix(t->taskbar, t->accent, 90));
    gfx_fill_rect(4, y + 4, START_W - 8, TASKBAR_H - 8, start_open ? gfx_mix(t->accent, 0xFFFFFF, 60) : t->accent);
    gfx_draw_text(4 + (START_W - 8 - gfx_text_width("Лимон", 1)) / 2, y + 8, "Лимон", t->on_accent, 1);

    int n = 0;
    for (int i = 0; i < MAX_WIN; i++) {
        if (!wins[i].used) continue;
        int bx = START_W + 4 + n * (TASKBTN_W + 4);
        uint32_t col = (i == focus) ? gfx_mix(t->taskbar, t->accent, 110) : wins[i].minimized ? t->taskbar : t->title_off;
        gfx_fill_rect(bx, y + 4, TASKBTN_W, TASKBAR_H - 8, col);
        if (i == focus) gfx_fill_rect(bx, y + TASKBAR_H - 6, TASKBTN_W, 2, t->accent);
        char label[24];
        int len = 0;
        const char *p = wins[i].title;
        while (*p && len < 17) { uint32_t cp = utf8_next(&p); len += utf8_encode(cp, label + len); }
        label[len] = 0;
        gfx_draw_text(bx + 8, y + 8, label, wins[i].minimized ? 0x8A94A6 : 0xE6EAF0, 1);
        n++;
    }

    char clock[8];
    ksnprintf(clock, sizeof clock, "%02d:%02d", now.hour, now.min);
    int cw = gfx_text_width(clock, 1);
    gfx_draw_text(dw - cw - 10, y + 8, clock, 0xE6EAF0, 1);
    const char *lang = kbd_layout() == LAYOUT_RU ? "RU" : "EN";
    int lx = dw - cw - 10 - 44;
    {
        const struct netinfo *ni = net_info();
        gfx_draw_text(lx - 36, y + 8, "NET", ni->configured ? 0x58D68D : 0x6B7685, 1);
    }
    gfx_fill_rect(lx, y + 5, 34, TASKBAR_H - 10, t->title_off);
    gfx_draw_text(lx + 9, y + 8, lang, t->accent, 1);
}

static void draw_menu(void) {
    const struct theme *t = T();
    int x, y, w, h;
    menu_rect(&x, &y, &w, &h);
    gfx_blend_rect(x + 4, y + 4, w, h, 0x000000, 90);
    gfx_fill_rect(x, y, w, h, t->taskbar);
    gfx_draw_rect(x, y, w, h, t->accent);
    for (int i = 0; i < MENU_N; i++) {
        int iy = y + 4 + i * MENU_ITEM_H;
        int hover = in_rect(mx, my, x + 2, iy, w - 4, MENU_ITEM_H);
        if (hover) gfx_fill_rect(x + 2, iy, w - 4, MENU_ITEM_H, t->accent);
        gfx_draw_text(x + 14, iy + 6, menu_items[i], hover ? t->on_accent : 0xE6EAF0, 1);
    }
}

static const char *const cursor_art[] = {
    "X          ", "XX         ", "X.X        ", "X..X       ", "X...X      ", "X....X     ",
    "X.....X    ", "X......X   ", "X.......X  ", "X........X ", "X.....XXXXX", "X..X..X    ",
    "X.X X..X   ", "XX  X..X   ", "X    X..X  ", "     XXX   ",
};

static void draw_cursor(void) {
    for (int j = 0; j < 16; j++)
        for (int i = 0; cursor_art[j][i]; i++) {
            char c = cursor_art[j][i];
            if (c == 'X') gfx_put_pixel(mx + i, my + j, 0x000000);
            else if (c == '.') gfx_put_pixel(mx + i, my + j, 0xFFFFFF);
        }
}

static void render(void) {
    draw_wallpaper();
    draw_icons();
    int focus = top_window();
    for (int i = 0; i < zcount; i++) {
        int idx = zorder[i];
        if (!wins[idx].minimized) draw_window(idx, idx == focus);
    }
    draw_taskbar(focus);
    if (start_open) draw_menu();
    draw_cursor();
}

void desktop_refresh(void) { redraw = 1; desktop_frame(); }

void desktop_frame(void) {
    if (!redraw) return;
    render();
    gfx_present();
    redraw = 0;
}

/* ---------- события ---------- */

static void menu_action(int item) {
    switch (item) {
    case 0: desktop_open_terminal(); break;
    case 1: desktop_open_about(); break;
    case 2: desktop_set_theme((cur_theme + 1) % NTHEMES); break;
    case 3: desktop_power(1); break;
    case 4: desktop_power(0); break;
    default: break;
    }
}

static void open_icon(int i) { if (i == 0) desktop_open_terminal(); else desktop_open_about(); }

static void handle_click(int px, int py) {
    int was_open = start_open;
    start_open = 0;

    if (was_open) {
        int x, y, w, h;
        menu_rect(&x, &y, &w, &h);
        if (in_rect(px, py, x, y, w, h)) {
            int item = (py - y - 4) / MENU_ITEM_H;
            if (item >= 0 && item < MENU_N) menu_action(item);
            return;
        }
    }

    if (py >= dh - TASKBAR_H) {
        if (px < START_W) { start_open = !was_open; return; }
        int n = 0, focus = top_window();
        for (int i = 0; i < MAX_WIN; i++) {
            if (!wins[i].used) continue;
            int bx = START_W + 4 + n * (TASKBTN_W + 4);
            if (in_rect(px, py, bx, dh - TASKBAR_H + 4, TASKBTN_W, TASKBAR_H - 8)) {
                if (wins[i].minimized || i != focus) focus_window(i);
                else wins[i].minimized = 1;
                return;
            }
            n++;
        }
        return;
    }

    int wi = win_at(px, py);
    if (wi >= 0) {
        struct window *w = &wins[wi];
        focus_window(wi);
        if (close_hit(w, px, py)) close_window(wi);
        else if (min_hit(w, px, py)) w->minimized = 1;
        else if (py < w->y + TITLE_H) { drag_win = wi; drag_dx = px - w->x; drag_dy = py - w->y; }
        return;
    }

    for (int i = 0; i < ICON_COUNT; i++) {
        int x, y;
        icon_rect(i, &x, &y);
        if (in_rect(px, py, x, y, 88, 90)) {
            if (last_click_icon == i && timer_ticks - last_click_tick < TIMER_HZ * 6 / 10) {
                open_icon(i);
                last_click_icon = -1;
            } else {
                last_click_icon = i;
                last_click_tick = timer_ticks;
            }
            return;
        }
    }
    last_click_icon = -1;
}

void desktop_mouse(const struct mouse_event *e) {
    mx = e->x; my = e->y;
    int left = e->buttons & 1;
    if (drag_win >= 0) {
        if (left) {
            struct window *w = &wins[drag_win];
            w->x = mx - drag_dx;
            w->y = my - drag_dy;
            if (w->y < 0) w->y = 0;
            if (w->y > dh - TASKBAR_H - TITLE_H) w->y = dh - TASKBAR_H - TITLE_H;
            if (w->x < 60 - w->w) w->x = 60 - w->w;
            if (w->x > dw - 60) w->x = dw - 60;
        } else drag_win = -1;
    } else if (left && !prev_left) {
        handle_click(mx, my);
    }
    prev_left = left;
    redraw = 1;
}

void desktop_key(const struct key_event *ev) {
    redraw = 1;
    uint32_t cp = ev->cp;
    if (cp == KEY_F(2)) { kbd_set_layout(kbd_layout() == LAYOUT_RU ? LAYOUT_EN : LAYOUT_RU); return; }
    if (cp == KEY_F(3)) { desktop_open_terminal(); return; }
    if (cp == KEY_F(4)) { int f = top_window(); if (f >= 0) close_window(f); return; }
    if (cp == KEY_F(5)) { desktop_set_theme((cur_theme + 1) % NTHEMES); return; }
    if (cp == 27 && start_open) { start_open = 0; return; }
    if ((ev->mods & MOD_ALT) && cp == '\t') {
        for (int i = 0; i < zcount; i++)         /* самое нижнее видимое окно — наверх */
            if (!wins[zorder[i]].minimized) { z_to_top(zorder[i]); break; }
        return;
    }
    int f = top_window();
    if (f >= 0 && wins[f].type == WIN_TERM) shell_key(wins[f].sh, ev);
}

void desktop_tick(void) {
    uint64_t sec = timer_ticks / TIMER_HZ;
    if (sec != last_sec) {
        last_sec = sec;
        int old_min = now.min;
        rtc_read(&now);
        if (now.min != old_min) redraw = 1;
    }
    int phase = (int)((timer_ticks / (TIMER_HZ / 2)) & 1);
    if (phase != blink_phase) { blink_phase = phase; redraw = 1; }
}

void desktop_power(int reboot) {
    gfx_clear(0x000000);
    const char *msg = reboot ? "LimonOS: перезагрузка..." : "LimonOS: выключение...";
    gfx_draw_text((int)gfx_width() / 2 - gfx_text_width(msg, 2) / 2, (int)gfx_height() / 2 - 16, msg, 0xF4D91A, 2);
    gfx_present();
    if (reboot) power_reboot();
    power_shutdown();
    gfx_clear(0x000000);
    msg = "Теперь можно выключить питание компьютера.";
    gfx_draw_text((int)gfx_width() / 2 - gfx_text_width(msg, 1) / 2, (int)gfx_height() / 2 - 8, msg, 0xF4D91A, 1);
    gfx_present();
    cpu_cli();
    for (;;) cpu_halt();
}

void desktop_init(void) {
    gfx_use_backbuffer();
    dw = (int)gfx_width();
    dh = (int)gfx_height();
    mouse_set_bounds(dw, dh);
    mx = dw / 2; my = dh / 2;
    rtc_read(&now);
    desktop_open_about();
    desktop_open_terminal();
    redraw = 1;
}

void desktop_run(void) {
    for (;;) {
        struct key_event k;
        struct mouse_event m;
        while (kbd_poll(&k)) desktop_key(&k);
        while (mouse_poll(&m)) desktop_mouse(&m);
        desktop_tick();
        desktop_frame();
        cpu_halt();
    }
}
