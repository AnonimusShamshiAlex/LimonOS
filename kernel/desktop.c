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
#include "http.h"
#include "tls.h"
#include "format.h"
#include "calc.h"
#include "kstring.h"
#include "mem.h"
#include "browser.h"
#include "games.h"
#include "vfs.h"

#define TASKBAR_H 32
#define TITLE_H   24
#define PAD       4
#define MAX_WIN   8
#define MAX_TERMS 4
#define MAX_BROWSERS 3
#define START_W   96
#define TASKBTN_W 150
#define BR_COLS   108
#define BR_ROWS   30
#define TOOL_H    30          /* панель с кнопками и адресом */
#define STAT_H    18          /* строка состояния внизу */
#define SB_W      10          /* полоса прокрутки */
#define BR_HIST   12

enum { WIN_TERM, WIN_ABOUT, WIN_BROWSER, WIN_CALC, WIN_GAME, WIN_FILES, WIN_TASKMGR };
#define MAX_GAMES 4

#define MAX_CALCS 2
#define CALC_W    220
#define CALC_H    300
#define CALC_COLS 4
#define CALC_ROWS 5
#define CALC_GAP  6
#define CALC_DISP_H 56

struct hit { int x, y, w, h, kind, idx; };        /* kind: 0 ссылка, 1 поле, 2 кнопка */

struct browser {
    struct page pg;
    int scroll;
    char cur[640];                  /* адрес открытой страницы */
    char addr[640]; int addr_focus; /* редактируемый адрес */
    char status[200];
    char hist[BR_HIST][400]; int hist_n, hist_pos;
    int focus_input;
    struct hit hits[220]; int nhits;
};

struct theme {
    const char *name;
    uint32_t bg_top, bg_bot, accent, on_accent, title_off, taskbar, body, term_bg, term_fg, accent2;
};

/* Тёмные темы. Первая (punk) — основная: кислотный лайм + розовый на почти чёрном.
   Старые темы (lemon, lime, orange, berry, night) остались: tint lemon и т.д. */
static const struct theme themes[] = {
    { "punk",   0x0C0C14, 0x1E0A26, 0xC6FF00, 0x000000, 0x2C2C3C, 0x08080D, 0x14141D, 0x07070C, 0xE6F7C2, 0xFF2E88 },
    { "riot",   0x140808, 0x050505, 0xFFE600, 0x000000, 0x3A2222, 0x0A0505, 0x1A1010, 0x080404, 0xF5E9C8, 0xFF2D2D },
    { "vapor",  0x0A1030, 0x28083A, 0x00F0FF, 0x00131A, 0x2A2E56, 0x080A1E, 0x121634, 0x070920, 0xD6F6FF, 0xFF3CAC },
    { "lemon",  0x2B3A4A, 0x10161E, 0xF4D91A, 0x14181F, 0x3A4554, 0x151B23, 0x1B222C, 0x0F141B, 0xD8DEE9, 0xFF6B3D },
    { "lime",   0x24402B, 0x0C1A10, 0x8BD13A, 0x10180A, 0x35503B, 0x0F1B13, 0x16241A, 0x0C140F, 0xDDEBDD, 0xE0568C },
    { "orange", 0x40291A, 0x180D08, 0xFF9F1C, 0x1A1008, 0x55392A, 0x1D120B, 0x2A1A10, 0x150C07, 0xF1E3D6, 0x2EC4B6 },
    { "berry",  0x3A2145, 0x140A1A, 0xE0568C, 0x1A0A12, 0x4E3560, 0x1A0F22, 0x251530, 0x110818, 0xEBDDF0, 0xC6FF00 },
    { "night",  0x16233A, 0x04070D, 0x6CB6FF, 0x08101A, 0x2B3C57, 0x0A111C, 0x101A2B, 0x070C14, 0xD6E4F5, 0xFF9F1C },
};
#define NTHEMES ((int)(sizeof themes / sizeof themes[0]))

struct window {
    int used, minimized, type;
    int x, y, w, h;
    char title[32];
    struct term *term;
    struct shell *sh;
    struct browser *br;
    struct calc_state *calc;
    struct game *game;
    int slot;
    int files_dir;                 /* WIN_FILES: папка, которую показываем */
    int files_sel;                 /* выбранная запись (-1 — нет) */
    int files_naming;              /* 1 — идёт ввод имени новой папки */
    char files_namebuf[48];
};

static struct window wins[MAX_WIN];
static int zorder[MAX_WIN], zcount;
static struct term terms[MAX_TERMS];
static struct shell shells[MAX_TERMS];
static uint8_t term_used[MAX_TERMS];
static struct browser browsers[MAX_BROWSERS];
static uint8_t browser_used[MAX_BROWSERS];
static struct calc_state calcs[MAX_CALCS];
static uint8_t calc_used[MAX_CALCS];
static struct game games[MAX_GAMES];
static uint8_t game_used[MAX_GAMES];
static int best_store[GAME_COUNT];            /* рекорды живут, пока система включена */
static int game_pending;                      /* игра изменилась, нужна перерисовка */
static uint64_t last_game_draw;
#define MAX_FILEWINS 2
static uint8_t fileswin_used[MAX_FILEWINS];
static int taskmgr_open;      /* не больше одного диспетчера задач */

/* раскладка кнопок калькулятора: что видно и какую команду шлёт в calc_* */
struct calc_btn { const char *label; char cmd; };
static const struct calc_btn calc_layout[CALC_ROWS][CALC_COLS] = {
    { {"C",'C'}, {"<-",'B'}, {"%",'%'}, {"/",'/'} },
    { {"7",'7'}, {"8",'8'}, {"9",'9'}, {"*",'*'} },
    { {"4",'4'}, {"5",'5'}, {"6",'6'}, {"-",'-'} },
    { {"1",'1'}, {"2",'2'}, {"3",'3'}, {"+",'+'} },
    { {"+/-",'N'}, {"0",'0'}, {".",'.'}, {"=",'='} },
};

static void calc_press(struct calc_state *s, char cmd) {
    if (cmd >= '0' && cmd <= '9') calc_digit(s, cmd);
    else if (cmd == '.') calc_dot(s);
    else if (cmd == 'C') calc_clear(s);
    else if (cmd == 'B') calc_backspace(s);
    else if (cmd == 'N') calc_negate(s);
    else if (cmd == '%') calc_percent(s);
    else if (cmd == '=') calc_equals(s);
    else calc_op(s, cmd);
}

static void calc_btn_rect(const struct window *w, int row, int col, int *bx, int *by, int *bw, int *bh) {
    int avail_w = CALC_W - 2 * CALC_GAP;
    int avail_h = CALC_H - CALC_DISP_H - (CALC_ROWS + 1) * CALC_GAP;
    int cw = (avail_w - (CALC_COLS - 1) * CALC_GAP) / CALC_COLS;
    int ch = avail_h / CALC_ROWS;
    *bw = cw; *bh = ch;
    *bx = w->x + CALC_GAP + col * (cw + CALC_GAP);
    *by = w->y + TITLE_H + CALC_DISP_H + CALC_GAP + row * (ch + CALC_GAP);
}

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

static const char home_html[] =
    "<h1>Цитрус 2</h1><p>Браузер LimonOS. Введи адрес сверху (Ctrl+L) или найди в интернете:</p>"
    "<form action=\"https://html.duckduckgo.com/html/\" method=\"get\"><input name=\"q\"><input type=\"submit\" value=\"Поиск\"></form>"
    "<p>Быстрые ссылки:</p><ul><li><a href=\"https://html.duckduckgo.com/html/\">DuckDuckGo</a></li>"
    "<li><a href=\"https://en.wikipedia.org/wiki/Lemon\">Wikipedia: Lemon</a></li>"
    "<li><a href=\"https://github.com/AnonimusShamshiAlex/LimonOS\">LimonOS на GitHub</a></li>"
    "<li><a href=\"https://example.com/\">example.com</a></li></ul>"
    "<hr><p>Умеет: ссылки, формы, заголовки, списки, windows-1251. Пока нет: JavaScript, CSS, картинки.</p>";

static void set_win_title(struct window *w, const char *title, const char *fallback) {
    const char *t = title[0] ? title : fallback;
    char buf[32] = "Цитрус: ";
    size_t o = strlen(buf);
    const char *p = t;
    while (*p) {
        const char *q = p; utf8_next(&q);
        if (o + (size_t)(q - p) >= sizeof buf) break;
        for (; p < q; p++) buf[o++] = *p;
    }
    buf[o] = 0;
    ksnprintf(w->title, sizeof w->title, "%s", buf);
}

static const char *url_host_part(const char *url) {
    for (const char *p = url; p[0] && p[1] && p[2]; p++)
        if (p[0] == ':' && p[1] == '/' && p[2] == '/') return p + 3;
    return url;
}

static int ct_is(const char *ct, const char *what) {
    for (const char *p = ct; *p; p++) {
        size_t j = 0;
        while (what[j] && p[j] && ((p[j] >= 'A' && p[j] <= 'Z') ? p[j] + 32 : p[j]) == what[j]) j++;
        if (!what[j]) return 1;
    }
    return 0;
}

static void hist_push(struct browser *b, const char *url) {
    if (strlen(url) >= sizeof b->hist[0]) return;
    b->hist_n = b->hist_pos + 1;
    if (b->hist_n >= BR_HIST) {
        for (int i = 1; i < BR_HIST; i++) memcpy(b->hist[i - 1], b->hist[i], sizeof b->hist[0]);
        b->hist_n = BR_HIST - 1;
    }
    memcpy(b->hist[b->hist_n], url, strlen(url) + 1);
    b->hist_pos = b->hist_n;
    b->hist_n++;
}

/* Загружает страницу. 0 — успех; -1 — не удалось (старая страница остаётся). */
static int browser_load(struct window *w, struct browser *b, const char *url0, int push) {
    char url[640];
    ksnprintf(url, sizeof url, "%s", url0);
    ksnprintf(b->status, sizeof b->status, "Загрузка %s ...", url);
    redraw = 1; desktop_frame();                 /* показать "Загрузка..." перед долгим запросом */

    for (int hop = 0; hop < 4; hop++) {
        struct http_opts o; memset(&o, 0, sizeof o);
        o.max_bytes = 2u << 20;                   /* страницы читаем до 2 МиБ */
        struct http_resp r;
        int rc = http_fetch(url, &o, &r);
        if (rc != HTTP_OK) {
            if (rc == HTTP_E_TLS) ksnprintf(b->status, sizeof b->status, "TLS: %s", tls_strerror(r.tls_err));
            else ksnprintf(b->status, sizeof b->status, "%s", http_strerror(rc));
            http_free(&r); redraw = 1; return -1;
        }
        if (r.status < 200 || r.status >= 400) {
            ksnprintf(b->status, sizeof b->status, "Сервер ответил %d", r.status);
            http_free(&r); redraw = 1; return -1;
        }

        const char *ct = r.content_type;
        const char *body = (const char *)r.data;
        size_t blen = r.len;
        int is_html = !ct[0] || ct_is(ct, "html");
        int is_text = ct_is(ct, "text/") || ct_is(ct, "json") || ct_is(ct, "xml") || ct_is(ct, "javascript");
        if (!is_html && !is_text) {
            ksnprintf(b->status, sizeof b->status, "Тип «%s» пока не открывается (%d байт). Скачать: grab", ct, (int)blen);
            http_free(&r); redraw = 1; return -1;
        }
        /* кодировка */
        char *conv = NULL; size_t clen = 0;
        int cp1251 = ct_is(ct, "1251");
        if (!cp1251 && !ct_is(ct, "charset")) {
            size_t head = blen < 2048 ? blen : 2048;
            char tmp[2049]; for (size_t i = 0; i < head; i++) tmp[i] = body[i]; tmp[head] = 0;
            cp1251 = ct_is(tmp, "windows-1251");
        }
        if (cp1251) { conv = cp1251_to_utf8(body, blen, &clen); if (conv) { body = conv; blen = clen; } }

        page_free(&b->pg);
        if (is_html) page_render(&b->pg, body, blen, BR_COLS, r.url);
        else page_render_text(&b->pg, body, blen, BR_COLS);
        if (conv) kfree(conv);

        b->scroll = 0; b->focus_input = -1; b->addr_focus = 0;
        ksnprintf(b->cur, sizeof b->cur, "%s", r.url);
        ksnprintf(b->status, sizeof b->status, "%s HTTP %d | %d КБ | строк %d",
                  r.https ? "https (сертификат не проверен)" : "http (без шифрования)", r.status, (int)(r.len + 1023) / 1024, b->pg.nlines);
        set_win_title(w, b->pg.title, url_host_part(r.url));
        if (push) hist_push(b, r.url);
        if (b->pg.refresh[0] && strcmp(b->pg.refresh, r.url) != 0 && hop < 3) {
            ksnprintf(url, sizeof url, "%s", b->pg.refresh);        /* <meta refresh>: идём дальше */
            push = 0;
            if (b->hist_pos >= 0 && b->hist_pos < b->hist_n) ksnprintf(b->hist[b->hist_pos], sizeof b->hist[0], "%s", url);
            http_free(&r);
            continue;
        }
        http_free(&r);
        break;
    }
    redraw = 1;
    return 0;
}

static void browser_show_home(struct window *w, struct browser *b) {
    page_free(&b->pg);
    page_render(&b->pg, home_html, sizeof home_html - 1, BR_COLS, "https://citrus.local/");
    b->scroll = 0; b->focus_input = -1; b->cur[0] = 0;
    ksnprintf(b->status, sizeof b->status, "Готов");
    set_win_title(w, "", "старт");
    if (b->hist_n == 0) { b->hist_pos = -1; }
}

/* то, что человек набрал в адресной строке: адрес или слова для поиска */
static int browser_go_typed(struct window *w, struct browser *b, const char *text) {
    char t[300]; size_t l = 0;
    while (*text == ' ') text++;
    while (*text && l < sizeof t - 1) t[l++] = *text++;
    while (l && t[l - 1] == ' ') l--;
    t[l] = 0;
    if (!l) return -1;
    char url[640];
    int has_space = 0, has_dot = 0;
    for (size_t i = 0; i < l; i++) { if (t[i] == ' ') has_space = 1; if (t[i] == '.') has_dot = 1; }
    if (strncmp(t, "http://", 7) == 0 || strncmp(t, "https://", 8) == 0) return browser_load(w, b, t, 1);
    if (has_space || (!has_dot && strncmp(t, "localhost", 9) != 0)) {
        char q[300]; url_encode(t, q, sizeof q);
        ksnprintf(url, sizeof url, "https://html.duckduckgo.com/html/?q=%s", q);
        return browser_load(w, b, url, 1);
    }
    ksnprintf(url, sizeof url, "https://%s", t);
    if (browser_load(w, b, url, 1) == 0) return 0;
    ksnprintf(url, sizeof url, "http://%s", t);                      /* сайт без https */
    return browser_load(w, b, url, 1);
}

int desktop_open_browser(const char *text) {
    int slot = -1;
    for (int i = 0; i < MAX_BROWSERS; i++) if (!browser_used[i]) { slot = i; break; }
    if (slot < 0) return -1;
    struct browser *b = &browsers[slot];
    memset(b, 0, sizeof *b);
    b->focus_input = -1; b->hist_pos = -1;
    browser_used[slot] = 1;
    int wi = alloc_window(WIN_BROWSER, BR_COLS * FONT_W + 2 * PAD + 2 + SB_W, TITLE_H + TOOL_H + BR_ROWS * FONT_H + STAT_H + 1, "Цитрус");
    if (wi < 0) { browser_used[slot] = 0; return -1; }
    wins[wi].br = b;
    wins[wi].slot = slot;
    wins[wi].x = 20; wins[wi].y = 10;
    focus_window(wi);
    browser_show_home(&wins[wi], b);
    if (text && text[0]) browser_go_typed(&wins[wi], b, text);
    else { b->addr_focus = 1; b->addr[0] = 0; }
    redraw = 1;
    return wi;
}

static void close_window(int idx);

static const char *win_kind_name(int type) {
    switch (type) { case WIN_TERM: return "терминал"; case WIN_ABOUT: return "информация"; case WIN_BROWSER: return "браузер";
        case WIN_CALC: return "калькулятор"; case WIN_GAME: return "игра"; case WIN_FILES: return "проводник"; case WIN_TASKMGR: return "диспетчер"; default: return "окно"; }
}

int desktop_task_count(void) { int n = 0; for (int i = 0; i < MAX_WIN; i++) if (wins[i].used) n++; return n; }

int desktop_task_info(int ord, char *title, size_t tcap, char *kind, size_t kcap, int *minimized, int *focused) {
    int n = 0, foc = top_window();
    for (int i = 0; i < MAX_WIN; i++) {
        if (!wins[i].used) continue;
        if (n == ord) {
            if (title) ksnprintf(title, tcap, "%s", wins[i].title);
            if (kind) ksnprintf(kind, kcap, "%s", win_kind_name(wins[i].type));
            if (minimized) *minimized = wins[i].minimized;
            if (focused) *focused = (i == foc);
            return 0;
        }
        n++;
    }
    return -1;
}

int desktop_task_close(int ord) {
    int n = 0;
    for (int i = 0; i < MAX_WIN; i++) {
        if (!wins[i].used) continue;
        if (n == ord) { close_window(i); redraw = 1; return 0; }
        n++;
    }
    return -1;
}

static void close_window(int idx) {
    struct window *w = &wins[idx];
    if (w->type == WIN_TERM) term_used[w->slot] = 0;
    else if (w->type == WIN_BROWSER) { page_free(&w->br->pg); browser_used[w->slot] = 0; }
    else if (w->type == WIN_CALC) calc_used[w->slot] = 0;
    else if (w->type == WIN_GAME) {
        for (int k = 0; k < GAME_COUNT; k++) if (w->game->best[k] > best_store[k]) best_store[k] = w->game->best[k];
        game_used[w->slot] = 0;
    }
    else if (w->type == WIN_FILES) fileswin_used[w->slot] = 0;
    else if (w->type == WIN_TASKMGR) taskmgr_open = 0;
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

#define ICON_COUNT 10
static const char *const icon_label[ICON_COUNT] = { "Терминал", "О системе", "Лимонка", "Цитрус", "Змейка", "Тетрис", "Понг", "2048", "Проводник", "Диспетчер" };
/* значки идут столбцами; сколько влезает в столбец — зависит от высоты экрана (на 800x600 их меньше, чем на 1024x768) */
static void icon_rect(int i, int *x, int *y) {
    int per = (dh - TASKBAR_H - 16) / 86;
    if (per < 1) per = 1;
    *x = 12 + (i / per) * 100;
    *y = 16 + (i % per) * 86;
}

static const char *const menu_items[] = { "Терминал", "Цитрус (браузер)", "Лимонка", "Проводник", "Диспетчер задач",
                                          "Змейка", "Тетрис", "Понг", "2048",
                                          "О системе", "Следующая тема", "Перезагрузка", "Выключение" };
#define MENU_N 13
#define MENU_W 230
#define MENU_ITEM_H 26
static void menu_rect(int *x, int *y, int *w, int *h) {
    *w = MENU_W; *h = MENU_N * MENU_ITEM_H + 8; *x = 0; *y = dh - TASKBAR_H - *h;
    if (*y < 0) *y = 0;
}

/* ---------- рисование ---------- */

static void draw_lemon(int cx, int cy, int r, uint32_t alpha) {
    gfx_fill_ellipse_a(cx, cy, r + r / 3, r, 0xF4D91A, alpha);
    gfx_fill_ellipse_a(cx - r / 3, cy - r / 3, r / 3, r / 4, 0xFFF9C4, alpha / 2);
    gfx_fill_ellipse_a(cx + r + r / 3, cy - r / 8, r / 8 + 1, r / 8 + 1, 0xE0C010, alpha);
    gfx_fill_ellipse_a(cx + r / 2, cy - r - r / 6, r / 3, r / 6 + 1, 0x4CAF50, alpha);
}

/* Талисман: лимон в тёмных очках. Рисуется один раз в кэш обоев. */
static void draw_lemon_mascot(int cx, int cy, int r) {
    const struct theme *t = T();
    gfx_fill_ellipse_a(cx, cy, r * 2, r * 2 - r / 4, t->accent2, 38);                    /* розовое сияние */
    gfx_fill_ellipse_a(cx, cy, r + r / 2 + r / 3, r + r / 5, t->accent, 30);
    gfx_fill_ellipse(cx + 6, cy + 6, r + r / 3 + 4, r + 4, 0x000000);                    /* жёсткая тень */
    gfx_fill_ellipse(cx, cy, r + r / 3 + 4, r + 4, 0x000000);                            /* контур */
    gfx_fill_ellipse(cx, cy, r + r / 3, r, 0xF4D91A);
    gfx_fill_ellipse(cx + r + r / 3, cy - r / 8, r / 8 + 5, r / 8 + 5, 0x000000);
    gfx_fill_ellipse(cx + r + r / 3, cy - r / 8, r / 8 + 1, r / 8 + 1, 0xE0C010);
    gfx_fill_ellipse(cx + r / 2, cy - r - r / 6, r / 3 + 4, r / 6 + 5, 0x000000);        /* лист */
    gfx_fill_ellipse(cx + r / 2, cy - r - r / 6, r / 3, r / 6 + 1, 0x39FF14);
    /* очки */
    int ly = cy - r / 6, lrx = r / 3 + 2, lry = r / 4;
    gfx_fill_rect(cx - r - r / 6, ly - lry - r / 12, 2 * (r + r / 6), r / 9 + 2, 0x000000);   /* дужка */
    gfx_fill_ellipse(cx - r / 2, ly, lrx, lry, 0x000000);
    gfx_fill_ellipse(cx + r / 2, ly, lrx, lry, 0x000000);
    gfx_fill_rect(cx - r / 2 + lrx - 2, ly - lry / 2, r - 2 * lrx + 4, lry / 3 + 2, 0x000000);  /* мостик */
    gfx_fill_parallelogram(cx - r / 2 - lrx / 2, ly - lry + 3, lrx / 4 + 2, lry * 2 - 6, 6, gfx_mix(0x000000, t->accent2, 150));   /* блики */
    gfx_fill_parallelogram(cx + r / 2 - lrx / 2, ly - lry + 3, lrx / 4 + 2, lry * 2 - 6, 6, gfx_mix(0x000000, t->accent2, 150));
    /* ухмылка */
    for (int dx = -r / 2; dx <= r / 2; dx++) {
        int yy = cy + r / 3 + (dx * dx) / (r > 0 ? r : 1) * 3 / 4 - r / 8;
        gfx_fill_rect(cx + dx, yy, 4, 4, 0x000000);
    }
    gfx_fill_ellipse(cx + r / 5, cy + r / 3 + r / 6, r / 8, r / 11 + 1, t->accent2);       /* язык */
}

static void paint_wallpaper(void) {
    const struct theme *t = T();
    gfx_vgradient(0, 0, dw, dh, t->bg_top, t->bg_bot);
    /* диагональные «полосы афиши» */
    for (int k = -2; k < dw / 170 + 3; k++)
        gfx_fill_parallelogram(k * 170, 0, 4, dh, -dh * 2 / 3, gfx_mix(t->bg_bot, t->accent2, 40));
    for (int k = -2; k < dw / 340 + 3; k++)
        gfx_fill_parallelogram(k * 340 + 60, 0, 22, dh, -dh * 2 / 3, gfx_mix(t->bg_top, t->accent, 22));
    /* лента внизу */
    int ty = dh - TASKBAR_H - 14;
    for (int x = -20; x < dw; x += 28) gfx_fill_parallelogram(x, ty, 14, 10, 10, gfx_mix(t->bg_bot, t->accent, 70));
    int r = dh / 7;
    draw_lemon_mascot(dw * 4 / 5, dh * 2 / 5, r);
    const char *mark = "LIMON OS";
    int tw = gfx_text_width(mark, 4), mx0 = dw - tw - 28, my0 = dh - TASKBAR_H - 92;
    gfx_draw_text(mx0 + 4, my0 + 3, mark, 0x00E5FF, 4);                                /* глитч: сдвинутые копии */
    gfx_draw_text(mx0 - 4, my0 + 3, mark, t->accent2, 4);
    gfx_draw_text(mx0, my0, mark, t->accent, 4);
    const char *tag = "LOUD. FAST. LIMON.";
    gfx_draw_text(dw - gfx_text_width(tag, 1) - 28, my0 + 4 * FONT_H + 6, tag, gfx_mix(t->bg_bot, 0xFFFFFF, 140), 1);
}

static uint32_t *wp_cache;
static size_t wp_pixels;
static int wp_theme = -1;

static void draw_wallpaper(void) {
    size_t n = gfx_surface_pixels();
    if (wp_cache && wp_theme == cur_theme && wp_pixels == n) { gfx_load_surface(wp_cache); return; }
    paint_wallpaper();
    if (!wp_cache || wp_pixels != n) {
        if (wp_cache) kfree(wp_cache);
        wp_cache = kmalloc(n * 4);
        wp_pixels = n;
    }
    if (wp_cache) { gfx_save_surface(wp_cache); wp_theme = cur_theme; }     /* не хватило памяти — просто рисуем каждый раз */
}

static void icon_art(int i, int px, int py, const struct theme *t) {
    gfx_fill_rect(px + 4, py + 4, 48, 48, t->accent2);                         /* жёсткая тень */
    switch (i) {
    case 0:
        gfx_fill_rect(px, py, 48, 48, 0x000000); gfx_draw_rect(px, py, 48, 48, t->accent);
        gfx_fill_rect(px, py, 48, 6, t->accent); gfx_draw_text(px + 8, py + 12, ">_", t->accent, 2); break;
    case 1:
        gfx_fill_rect(px, py, 48, 48, 0x000000); gfx_fill_ellipse(px + 24, py + 24, 21, 21, t->accent);
        gfx_draw_text(px + 16, py + 8, "i", t->on_accent, 2); break;
    case 2:
        gfx_fill_rect(px, py, 48, 48, 0x0B0B10); gfx_draw_rect(px, py, 48, 48, t->accent);
        gfx_fill_rect(px + 5, py + 5, 38, 9, t->term_bg); gfx_draw_rect(px + 5, py + 5, 38, 9, t->accent2);
        for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) gfx_fill_rect(px + 6 + c * 9, py + 19 + r * 9, 7, 6, t->accent);
        break;
    case 3:
        gfx_fill_rect(px, py, 48, 48, 0xF2F2F2); gfx_fill_rect(px, py, 48, 6, t->accent); gfx_draw_rect(px, py, 48, 48, 0x000000);
        gfx_fill_ellipse(px + 24, py + 28, 16, 16, 0x2F7BFF);
        gfx_fill_rect(px + 8, py + 27, 33, 2, 0xFFFFFF); gfx_fill_rect(px + 23, py + 12, 2, 33, 0xFFFFFF); break;
    case 4:
        gfx_fill_rect(px, py, 48, 48, 0x05060A); gfx_draw_rect(px, py, 48, 48, 0x39FF14);
        { static const int cx[7] = { 5, 13, 21, 21, 21, 29, 37 }, cy[7] = { 33, 33, 33, 25, 17, 17, 17 };
          for (int k = 0; k < 7; k++) gfx_fill_rect(px + cx[k], py + cy[k], 7, 7, k == 6 ? 0xFFFFFF : 0x39FF14);
          gfx_fill_rect(px + 6, py + 8, 7, 7, t->accent2); } break;
    case 5:
        gfx_fill_rect(px, py, 48, 48, 0x05060A); gfx_draw_rect(px, py, 48, 48, 0xB84DFF);
        { static const struct { int x, y; uint32_t c; } b[] = {
            {8,8,0x00E5FF},{8,16,0x00E5FF},{8,24,0x00E5FF},{8,32,0x00E5FF}, {24,24,0xFFE600},{32,24,0xFFE600},{24,32,0xFFE600},{32,32,0xFFE600},
            {16,32,0xFF2D55},{16,24,0xFF2D55},{24,16,0xB84DFF},{32,8,0x39FF14},{32,16,0x39FF14} };
          for (unsigned k = 0; k < sizeof b / sizeof b[0]; k++) { gfx_fill_rect(px + b[k].x, py + b[k].y, 8, 8, 0x000000); gfx_fill_rect(px + b[k].x + 1, py + b[k].y + 1, 6, 6, b[k].c); } } break;
    case 6:
        gfx_fill_rect(px, py, 48, 48, 0x05060A); gfx_draw_rect(px, py, 48, 48, t->accent);
        for (int j = 4; j < 44; j += 8) gfx_fill_rect(px + 23, py + j, 2, 4, 0x3A3A4A);
        gfx_fill_rect(px + 5, py + 10, 4, 16, t->accent); gfx_fill_rect(px + 39, py + 24, 4, 16, t->accent2);
        gfx_fill_rect(px + 26, py + 20, 5, 5, 0xFFFFFF); break;
    case 7:
        gfx_fill_rect(px, py, 48, 48, 0xFF8A00); gfx_draw_rect(px, py, 48, 48, 0x000000);
        gfx_draw_text(px + 8, py + 16, "2048", 0x000000, 1); gfx_draw_text(px + 7, py + 15, "2048", 0xFFFFFF, 1);
        gfx_fill_rect(px + 6, py + 6, 8, 4, 0x000000); gfx_fill_rect(px + 6, py + 38, 36, 4, 0x000000); break;
    case 8:
        gfx_fill_rect(px, py, 48, 48, 0x000000); gfx_draw_rect(px, py, 48, 48, t->accent);
        gfx_fill_rect(px + 4, py + 10, 18, 7, t->accent2); gfx_fill_rect(px + 4, py + 16, 40, 28, gfx_mix(t->accent2, 0x000000, 60));
        gfx_draw_rect(px + 4, py + 16, 40, 28, t->accent2);
        gfx_fill_rect(px + 9, py + 24, 10, 12, 0x05060A); gfx_fill_rect(px + 23, py + 24, 16, 8, 0x05060A); break;
    default:
        gfx_fill_rect(px, py, 48, 48, 0x05060A); gfx_draw_rect(px, py, 48, 48, t->accent2);
        gfx_fill_rect(px + 7, py + 8, 34, 8, t->accent); gfx_fill_rect(px + 7, py + 20, 22, 6, t->accent2);
        gfx_fill_rect(px + 7, py + 30, 30, 6, gfx_mix(t->accent, t->accent2, 128));
        gfx_fill_rect(px + 30, py + 20, 11, 16, 0xFF3355); gfx_draw_text(px + 32, py + 22, "x", 0xFFFFFF, 1); break;
    }
}

static void draw_icons(void) {
    const struct theme *t = T();
    for (int i = 0; i < ICON_COUNT; i++) {
        int x, y;
        icon_rect(i, &x, &y);
        if (in_rect(mx, my, x, y, 88, 90)) gfx_blend_rect(x, y, 88, 90, t->accent, 40);
        icon_art(i, x + 18, y + 4, t);
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

static uint32_t pg_color(int c, int dark) {
    if (dark) {
        switch (c) {
        case PG_C_HEAD: return 0xFFD84D;
        case PG_C_LINK: return 0x8AB4F8;
        case PG_C_DIM:  return 0x9AA0A6;
        case PG_C_CODE: return 0x9BE564;
        default:        return 0xE6E6E6;
        }
    }
    switch (c) {
    case PG_C_HEAD: return 0x0B3D91;
    case PG_C_LINK: return 0x0B57D0;
    case PG_C_DIM:  return 0x6B7280;
    case PG_C_CODE: return 0x0B6B2A;
    default:        return 0x1F2328;
    }
}

static void add_hit(struct browser *b, int x, int y, int w, int h, int kind, int idx) {
    if (b->nhits >= (int)(sizeof b->hits / sizeof b->hits[0])) return;
    b->hits[b->nhits++] = (struct hit){ x, y, w, h, kind, idx };
}

static uint32_t hex6(const char *p) {
    uint32_t v = 0;
    for (int i = 0; i < 6; i++) { char c = p[i]; v = v * 16 + (uint32_t)(c <= '9' ? c - '0' : c - 'A' + 10); }
    return v;
}

static void draw_page_line(struct browser *b, int x, int y, const char *line, int focused) {
    const struct theme *t = T();
    int dark = b->pg.dark;
    uint32_t fg0 = pg_color(PG_C_TEXT, dark);
    int px = x, link = -1, link_x0 = 0, bold = 0;
    uint32_t col = fg0;
    const char *p = line;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c == PGM_COLOR) { col = pg_color(p[1] - '0', dark); p += 2; continue; }
        if (c == PGM_RGB) { col = hex6(p + 1); p += 7; continue; }
        if (c == PGM_BOLD) { bold = (p[1] == '1'); p += 2; continue; }
        if (c == PGM_LINK) { link = ((p[1] - '0') << 6) | (p[2] - '0'); link_x0 = px; p += 3; continue; }
        if (c == PGM_LINKEND) {
            if (link >= 0 && px > link_x0) { gfx_fill_rect(link_x0, y + FONT_H - 2, px - link_x0, 1, col); add_hit(b, link_x0, y, px - link_x0, FONT_H, 0, link); }
            link = -1; p++; continue;
        }
        if (c == PGM_INPUT) {
            int idx = ((p[1] - '0') << 6) | (p[2] - '0'); p += 3;
            int wpx = PG_FIELD_COLS * FONT_W;
            int foc = focused && b->focus_input == idx;
            gfx_fill_rect(px, y - 1, wpx, FONT_H + 2, dark ? gfx_mix(b->pg.bg, 0xFFFFFF, 14) : 0xFFFFFF);
            gfx_draw_rect(px, y - 1, wpx, FONT_H + 2, foc ? t->accent : (dark ? 0x6A6F76 : 0x8A8F96));
            if (foc) gfx_draw_rect(px + 1, y, wpx - 2, FONT_H, t->accent);
            const struct pg_input *in = &b->pg.inputs[idx];
            char shown[100]; int len = (int)strlen(in->value); if (len > 98) len = 98;
            if (in->password) { for (int i = 0; i < len; i++) shown[i] = '*'; shown[len] = 0; }
            else memcpy(shown, in->value, (size_t)len + 1);
            const char *s = shown; int nch = utf8_len(s);
            while (nch > PG_FIELD_COLS - 2) { utf8_next(&s); nch--; }
            int ex = gfx_draw_text(px + 4, y, s, fg0, 1);
            if (foc && blink_phase == 0) gfx_fill_rect(ex, y, FONT_W, FONT_H, t->accent);
            add_hit(b, px, y, wpx, FONT_H, 1, idx);
            px += wpx; continue;
        }
        if (c == PGM_BUTTON) {
            int idx = ((p[1] - '0') << 6) | (p[2] - '0'); p += 3;
            const char *lab = b->pg.inputs[idx].value;
            int wpx = (utf8_len(lab) + 4) * FONT_W;
            gfx_fill_rect(px, y - 1, wpx, FONT_H + 2, t->accent);
            gfx_draw_text(px + 2 * FONT_W, y, lab, t->on_accent, 1);
            add_hit(b, px, y, wpx, FONT_H, 2, idx);
            px += wpx; continue;
        }
        if (c < 0x20) { p++; continue; }
        const char *q = p;
        uint32_t cp = utf8_next(&q);
        gfx_draw_glyph(px, y, cp, col, 1);
        if (bold) gfx_draw_glyph(px + 1, y, cp, col, 1);          /* «жирный» — двойной удар со сдвигом на пиксель */
        px += FONT_W; p = q;
    }
    if (link >= 0 && px > link_x0) { gfx_fill_rect(link_x0, y + FONT_H - 2, px - link_x0, 1, col); add_hit(b, link_x0, y, px - link_x0, FONT_H, 0, link); }
}

static void draw_calc_content(struct window *w, int focused) {
    const struct theme *t = T();
    struct calc_state *s = w->calc;
    int y1 = w->y + TITLE_H;
    gfx_fill_rect(w->x + 1, y1, w->w - 2, w->h - TITLE_H - 1, t->body);

    gfx_fill_rect(w->x + CALC_GAP, y1 + CALC_GAP, CALC_W - 2 * CALC_GAP, CALC_DISP_H - CALC_GAP, t->term_bg);
    uint32_t dcolor = s->error ? 0xFF6B6B : t->term_fg;
    int tw = gfx_text_width(s->display, 2);
    int tx = w->x + CALC_W - CALC_GAP - 10 - tw; if (tx < w->x + CALC_GAP + 6) tx = w->x + CALC_GAP + 6;
    gfx_draw_text(tx, y1 + CALC_GAP + (CALC_DISP_H - CALC_GAP - FONT_H * 2) / 2, s->display, dcolor, 2);

    for (int r = 0; r < CALC_ROWS; r++) {
        for (int c = 0; c < CALC_COLS; c++) {
            int bx, by, bw, bh; calc_btn_rect(w, r, c, &bx, &by, &bw, &bh);
            char cmd = calc_layout[r][c].cmd;
            uint32_t base;
            if (cmd == '=') base = t->accent;
            else if (cmd == 'C' || cmd == 'B') base = gfx_mix(t->title_off, 0xFF6B6B, 30);
            else if (cmd == '+' || cmd == '-' || cmd == '*' || cmd == '/') base = gfx_mix(t->title_off, t->accent, 40);
            else base = t->title_off;
            int hover = focused && in_rect(mx, my, bx, by, bw, bh);
            gfx_fill_rect(bx, by, bw, bh, hover ? gfx_mix(base, 0xFFFFFF, 25) : base);
            const char *lab = calc_layout[r][c].label;
            int lw = gfx_text_width(lab, 1);
            uint32_t tc = (cmd == '=') ? t->on_accent : 0xE6EAF0;
            gfx_draw_text(bx + (bw - lw) / 2, by + (bh - FONT_H) / 2, lab, tc, 1);
        }
    }
}

static void draw_browser_content(struct window *w, int focused) {
    const struct theme *t = T();
    struct browser *b = w->br;
    int x0 = w->x + 1, ty = w->y + TITLE_H;
    int cy = ty + TOOL_H;                          /* верх области страницы */
    int cw = w->w - 2 - SB_W;
    /* панель инструментов */
    gfx_fill_rect(x0, ty, w->w - 2, TOOL_H, t->body);
    const char *lab[3] = { "<", ">", "R" };
    for (int i = 0; i < 3; i++) {
        int bx = x0 + 6 + i * 28;
        int en = (i == 0) ? b->hist_pos > 0 : (i == 1) ? b->hist_pos + 1 < b->hist_n : 1;
        gfx_fill_rect(bx, ty + 4, 24, 22, gfx_mix(t->body, 0xFFFFFF, en ? 22 : 8));
        gfx_draw_text(bx + 8, ty + 6, lab[i], en ? t->term_fg : gfx_mix(t->body, 0xFFFFFF, 35), 1);
    }
    int ax = x0 + 6 + 3 * 28 + 4, aw = w->w - 2 - (ax - x0) - 8;
    gfx_fill_rect(ax, ty + 4, aw, 22, gfx_mix(t->term_bg, 0xFFFFFF, 10));
    gfx_draw_rect(ax, ty + 4, aw, 22, (b->addr_focus && focused) ? t->accent : gfx_mix(t->body, 0xFFFFFF, 40));
    {
        const char *txt = b->addr_focus ? b->addr : b->cur;
        int maxch = aw / FONT_W - 2;
        const char *s = txt; int nch = utf8_len(s);
        if (b->addr_focus) { while (nch > maxch - 1) { utf8_next(&s); nch--; } }
        else { /* начало адреса, лишнее обрезаем */ }
        char tmp[200]; size_t o = 0; int cnt = 0;
        while (*s && cnt < maxch && o + 5 < sizeof tmp) { const char *q = s; utf8_next(&q); for (; s < q; s++) tmp[o++] = *s; cnt++; }
        tmp[o] = 0;
        int ex = gfx_draw_text(ax + 6, ty + 7, tmp[0] || b->addr_focus ? tmp : "введи адрес или слова для поиска", tmp[0] ? t->term_fg : gfx_mix(t->body, 0xFFFFFF, 50), 1);
        if (b->addr_focus && focused && blink_phase == 0) gfx_fill_rect(ex, ty + 7, FONT_W, FONT_H, t->accent);
    }
    /* страница */
    uint32_t page_bg = b->pg.bg ? b->pg.bg : 0xFFFFFF;
    gfx_fill_rect(x0, cy, w->w - 2, BR_ROWS * FONT_H + 1, page_bg);
    b->nhits = 0;
    int visible = BR_ROWS;
    for (int r = 0; r < visible; r++) {
        int li = b->scroll + r;
        if (li >= b->pg.nlines) break;
        draw_page_line(b, x0 + PAD, cy + r * FONT_H, b->pg.lines[li], focused);
    }
    if (b->pg.nlines > visible) {
        int track_h = visible * FONT_H;
        int thumb_h = visible * track_h / b->pg.nlines;
        if (thumb_h < 14) thumb_h = 14;
        int max_scroll = b->pg.nlines - visible;
        int thumb_y = cy + (track_h - thumb_h) * b->scroll / max_scroll;
        int bar_x = x0 + cw + 2;
        gfx_fill_rect(bar_x, cy, SB_W - 2, track_h, gfx_mix(page_bg, 0x808080, 40));
        gfx_fill_rect(bar_x, thumb_y, SB_W - 2, thumb_h, focused ? t->accent : t->title_off);
    }
    /* строка состояния: адрес ссылки под мышью или итог загрузки */
    int sy = cy + BR_ROWS * FONT_H + 1;
    gfx_fill_rect(x0, sy, w->w - 2, STAT_H, t->body);
    const char *st = b->status;
    for (int i = 0; i < b->nhits; i++) {
        struct hit *h = &b->hits[i];
        if (h->kind == 0 && in_rect(mx, my, h->x, h->y, h->w, h->h)) { st = b->pg.links[h->idx]; break; }
    }
    char sbuf[140]; size_t o = 0; int cnt = 0, maxc = (w->w - 16) / FONT_W;
    while (*st && cnt < maxc && o + 5 < sizeof sbuf) { const char *q = st; utf8_next(&q); for (; st < q; st++) sbuf[o++] = *st; cnt++; }
    sbuf[o] = 0;
    gfx_draw_text(x0 + 6, sy + 1, sbuf, t->accent, 1);
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

/* ---------- Проводник (Memoria: Desktop/Downloads/Music/Pictures/System) ---------- */
#define FILES_TOOL_H 28
#define FILES_STAT_H 20
#define FILES_ROW_H  20

struct files_entry { int idx; char name[48]; int is_dir; size_t size; };
#define FILES_MAX_ROWS 32
struct files_collect { struct files_entry arr[FILES_MAX_ROWS]; int n, more; };

static void files_collect_cb(int idx, const char *name, int is_dir, size_t sz, void *ctx) {
    struct files_collect *c = ctx;
    if (c->n < FILES_MAX_ROWS) {
        c->arr[c->n].idx = idx; ksnprintf(c->arr[c->n].name, sizeof c->arr[c->n].name, "%s", name);
        c->arr[c->n].is_dir = is_dir; c->arr[c->n].size = sz; c->n++;
    } else c->more = 1;
}

static void draw_files_content(struct window *w) {
    const struct theme *t = T();
    int x0 = w->x + 1, y0 = w->y + TITLE_H;
    int cw = w->w - 2, ch = w->h - TITLE_H - 1;
    gfx_fill_rect(x0, y0, cw, ch, t->body);
    uint32_t panel = gfx_mix(t->body, 0x000000, 50);
    gfx_fill_rect(x0, y0, cw, FILES_TOOL_H, panel);
    gfx_fill_rect(x0, y0 + FILES_TOOL_H - 1, cw, 1, t->accent2);
    int up_en = w->files_dir != VFS_ROOT;
    gfx_fill_rect(x0 + 4, y0 + 4, 20, 20, up_en ? t->accent : gfx_mix(panel, 0x000000, 40));
    gfx_draw_text(x0 + 10, y0 + 9, "^", up_en ? t->on_accent : 0x555566, 1);
    char path[160]; vfs_path(w->files_dir, path, sizeof path);
    gfx_draw_text(x0 + 30, y0 + 9, path, t->term_fg, 1);
    const char *hint = "N папка   Del удалить   F2 переим.";
    gfx_draw_text(x0 + cw - gfx_text_width(hint, 1) - 6, y0 + 9, hint, gfx_mix(t->body, 0xFFFFFF, 110), 1);

    int ly = y0 + FILES_TOOL_H;
    int rows_h = ch - FILES_TOOL_H - FILES_STAT_H;
    gfx_fill_rect(x0, ly, cw, rows_h, t->body);
    struct files_collect c; c.n = 0; c.more = 0;
    vfs_list(w->files_dir, files_collect_cb, &c);
    int maxrows = rows_h / FILES_ROW_H;
    int shown = c.n < maxrows ? c.n : maxrows;
    for (int i = 0; i < shown; i++) {
        int ry = ly + i * FILES_ROW_H;
        if (c.arr[i].idx == w->files_sel) gfx_fill_rect(x0 + 2, ry, cw - 4, FILES_ROW_H, gfx_mix(t->body, t->accent, 60));
        uint32_t col = c.arr[i].is_dir ? t->accent : t->term_fg;
        gfx_draw_text(x0 + 8, ry + 2, c.arr[i].is_dir ? "[папка]" : "[файл]", col, 1);
        char nm[40]; ksnprintf(nm, sizeof nm, "%.34s", c.arr[i].name);
        gfx_draw_text(x0 + 70, ry + 2, nm, t->term_fg, 1);
        if (!c.arr[i].is_dir) { char sz[16]; ksnprintf(sz, sizeof sz, "%u Б", (unsigned)c.arr[i].size); gfx_draw_text(x0 + cw - gfx_text_width(sz, 1) - 8, ry + 2, sz, gfx_mix(t->body, 0xFFFFFF, 130), 1); }
    }
    if (!c.n) gfx_draw_text(x0 + 8, ly + 4, "Пусто.", gfx_mix(t->body, 0xFFFFFF, 110), 1);
    else if (c.n > shown || c.more) gfx_draw_text(x0 + 8, ly + rows_h - FONT_H, "… есть ещё — удали что-нибудь видимое или открой подпапку", gfx_mix(t->body, 0xFFFFFF, 110), 1);

    int sy = ly + rows_h;
    gfx_fill_rect(x0, sy, cw, FILES_STAT_H, panel);
    if (w->files_naming) {
        gfx_draw_text(x0 + 6, sy + 3, "Имя новой папки:", t->accent, 1);
        int tx = x0 + 6 + 17 * FONT_W;
        gfx_fill_rect(tx, sy + 1, cw - (tx - x0) - 6, FILES_STAT_H - 2, gfx_mix(panel, 0xFFFFFF, 30));
        int ex = gfx_draw_text(tx + 3, sy + 3, w->files_namebuf, t->term_fg, 1);
        if (blink_phase == 0) gfx_fill_rect(ex, sy + 3, FONT_W, FONT_H, t->accent);
    } else {
        char st[64]; ksnprintf(st, sizeof st, "%d записей   Memoria всего: %u КиБ (до выключения)", c.n, (unsigned)(vfs_total_bytes() / 1024));
        gfx_draw_text(x0 + 6, sy + 3, st, gfx_mix(t->body, 0xFFFFFF, 130), 1);
    }
}

static void files_click(struct window *w, int px, int py) {
    int x0 = w->x + 1, y0 = w->y + TITLE_H;
    if (in_rect(px, py, x0 + 4, y0 + 4, 20, 20)) { if (w->files_dir != VFS_ROOT) { w->files_dir = vfs_parent(w->files_dir); w->files_sel = -1; } return; }
    int ly = y0 + FILES_TOOL_H, ch = w->h - TITLE_H - 1, rows_h = ch - FILES_TOOL_H - FILES_STAT_H;
    if (py < ly || py >= ly + rows_h) return;
    int row = (py - ly) / FILES_ROW_H;
    struct files_collect c; c.n = 0; c.more = 0;
    vfs_list(w->files_dir, files_collect_cb, &c);
    if (row < 0 || row >= c.n) { w->files_sel = -1; return; }
    int idx = c.arr[row].idx;
    static int last_row = -1; static int last_win = -1;
    if (last_win == (int)(w - wins) && last_row == row && timer_ticks - last_click_tick < TIMER_HZ * 6 / 10) {
        if (c.arr[row].is_dir) { w->files_dir = idx; w->files_sel = -1; }
    } else { w->files_sel = idx; }
    last_row = row; last_win = (int)(w - wins); last_click_tick = timer_ticks;
}

static void str_backspace(char *s);
static void str_append_cp(char *s, size_t max, uint32_t cp);

static void files_key(struct window *w, const struct key_event *ev) {
    uint32_t cp = ev->cp;
    if (w->files_naming) {
        if (cp == '\n') {
            if (w->files_namebuf[0] && vfs_mkdir(w->files_dir, w->files_namebuf) != VFS_NONE) w->files_naming = 0;
        } else if (cp == 27) w->files_naming = 0;
        else if (cp == '\b') str_backspace(w->files_namebuf);
        else if (cp >= 32 && cp < 0x110000 && !(ev->mods & (MOD_CTRL | MOD_ALT))) str_append_cp(w->files_namebuf, sizeof w->files_namebuf, cp);
        return;
    }
    if (cp == 'n' || cp == 'N' || cp == 0x442 /* т */) { w->files_naming = 1; w->files_namebuf[0] = 0; return; }
    if (cp == KEY_DELETE && w->files_sel >= 0) { vfs_delete(w->files_sel); w->files_sel = -1; return; }
    if (cp == '\b' && w->files_dir != VFS_ROOT) { w->files_dir = vfs_parent(w->files_dir); w->files_sel = -1; return; }
    if (cp == '\n' && w->files_sel >= 0 && vfs_is_dir(w->files_sel)) { w->files_dir = w->files_sel; w->files_sel = -1; }
}

/* ---------- Диспетчер задач ---------- */
#define TM_ROW_H 22
static void draw_taskmgr_content(struct window *w) {
    const struct theme *t = T();
    int x0 = w->x + 1, y0 = w->y + TITLE_H, cw = w->w - 2, ch = w->h - TITLE_H - 1;
    gfx_fill_rect(x0, y0, cw, ch, t->body);
    gfx_draw_text(x0 + 8, y0 + 6, "ОКНА", t->accent, 1);
    int n = desktop_task_count();
    int ly = y0 + 22;
    for (int i = 0; i < n && ly + TM_ROW_H < y0 + ch - 110; i++, ly += TM_ROW_H) {
        char title[40], kind[24]; int min, foc;
        desktop_task_info(i, title, sizeof title, kind, sizeof kind, &min, &foc);
        if (foc) gfx_fill_rect(x0 + 4, ly, cw - 8, TM_ROW_H - 2, gfx_mix(t->body, t->accent, 50));
        gfx_draw_text(x0 + 8, ly + 3, title, t->term_fg, 1);
        gfx_draw_text(x0 + 180, ly + 3, kind, gfx_mix(t->body, 0xFFFFFF, 130), 1);
        if (min) gfx_draw_text(x0 + 300, ly + 3, "свёрнуто", t->accent2, 1);
        gfx_fill_rect(x0 + cw - 26, ly, 20, TM_ROW_H - 2, gfx_mix(0x000000, t->accent2, 150));
        gfx_draw_text(x0 + cw - 21, ly + 3, "×", 0xFFFFFF, 1);
    }
    if (!n) gfx_draw_text(x0 + 8, ly + 3, "Открытых окон нет.", gfx_mix(t->body, 0xFFFFFF, 110), 1);

    int by = y0 + ch - 100;
    gfx_fill_rect(x0, by, cw, 1, t->accent2);
    gfx_draw_text(x0 + 8, by + 8, "ПАМЯТЬ", t->accent, 1);
    char l1[72], l2[72], l3[72];
    ksnprintf(l1, sizeof l1, "Куча:    %u / %u КиБ (растёт автоматически)", (unsigned)(heap_used() / 1024), (unsigned)(heap_total() / 1024));
    if (g_sys.mem_valid) ksnprintf(l2, sizeof l2, "ОЗУ:     %llu МиБ свободно", (unsigned long long)(g_sys.mem_usable >> 20));
    else ksnprintf(l2, sizeof l2, "ОЗУ:     карта памяти недоступна");
    ksnprintf(l3, sizeof l3, "Memoria: %u КиБ (Desktop/Downloads/Music/Pictures)", (unsigned)(vfs_total_bytes() / 1024));
    gfx_draw_text(x0 + 8, by + 26, l1, t->term_fg, 1);
    gfx_draw_text(x0 + 8, by + 42, l2, t->term_fg, 1);
    gfx_draw_text(x0 + 8, by + 58, l3, t->term_fg, 1);
    char up[16]; uint64_t sec = timer_ticks / TIMER_HZ;
    ksnprintf(up, sizeof up, "%02d:%02d:%02d", (int)(sec / 3600), (int)(sec / 60 % 60), (int)(sec % 60));
    gfx_draw_text(x0 + 8, by + 74, "Процессор:", gfx_mix(t->body, 0xFFFFFF, 130), 1);
    gfx_draw_text(x0 + 90, by + 74, g_sys.cpu.brand[0] ? g_sys.cpu.brand : g_sys.cpu.vendor, t->term_fg, 1);
    char upl[32]; ksnprintf(upl, sizeof upl, "Работает: %s", up);
    gfx_draw_text(x0 + cw - gfx_text_width(upl, 1) - 8, by + 74, upl, gfx_mix(t->body, 0xFFFFFF, 130), 1);
}

static void taskmgr_click(struct window *w, int px, int py) {
    int x0 = w->x + 1, y0 = w->y + TITLE_H;
    int ly = y0 + 22;
    int n = desktop_task_count();
    for (int i = 0; i < n; i++, ly += TM_ROW_H) {
        if (in_rect(px, py, x0 + (w->w - 2) - 26, ly, 20, TM_ROW_H - 2)) { desktop_task_close(i); return; }
        char title[40], kind[24]; int min, foc;
        desktop_task_info(i, title, sizeof title, kind, sizeof kind, &min, &foc);
        if (in_rect(px, py, x0 + 4, ly, w->w - 2 - 8, TM_ROW_H - 2)) {
            int ord = i, cnt = 0;
            for (int k = 0; k < MAX_WIN; k++) { if (!wins[k].used) continue; if (cnt == ord) { focus_window(k); if (wins[k].minimized) wins[k].minimized = 0; return; } cnt++; }
        }
    }
}

static void draw_game_content(struct window *w) {
    const struct theme *t = T();
    struct game_pal p = { 0x07070C, 0x15151F, 0xF2F2F2, 0x8A8FA3, t->accent, t->accent2, 0x39FF14, 0xFF3355 };
    gfx_fill_rect(w->x + 1, w->y + TITLE_H, w->w - 2, w->h - TITLE_H - 1, p.bg);
    game_draw(w->game, w->x + 1, w->y + TITLE_H, &p);
}

static void draw_window(int idx, int focused) {
    const struct theme *t = T();
    struct window *w = &wins[idx];
    uint32_t bar = focused ? t->accent : t->title_off;
    uint32_t bar_text = focused ? t->on_accent : 0xC8CED8;
    gfx_fill_rect(w->x + 6, w->y + 6, w->w, w->h, focused ? t->accent2 : 0x000000);       /* жёсткая тень (как на афише) */
    gfx_fill_rect(w->x - 2, w->y - 2, w->w + 4, w->h + 4, 0x000000);                      /* чёрный контур */
    gfx_fill_rect(w->x, w->y, w->w, w->h, bar);
    gfx_fill_rect(w->x + 1, w->y + TITLE_H, w->w - 2, w->h - TITLE_H - 1, t->body);
    gfx_fill_rect(w->x, w->y + TITLE_H - 2, w->w, 2, 0x000000);
    int tx = gfx_draw_text(w->x + 8, w->y + 4, w->title, bar_text, 1);
    gfx_draw_text(w->x + 9, w->y + 4, w->title, bar_text, 1);                             /* «жирный» заголовок */
    uint32_t stripe = gfx_mix(bar, 0x000000, focused ? 70 : 90);
    for (int sx = tx + 14; sx < w->x + w->w - 52; sx += 8)                                /* штриховка заголовка */
        gfx_fill_parallelogram(sx, w->y + 6, 4, TITLE_H - 12, 4, stripe);
    int closing = close_hit(w, mx, my), mining = min_hit(w, mx, my);
    gfx_fill_rect(w->x + w->w - 22, w->y + 4, 18, 16, closing ? t->accent2 : 0x000000);
    gfx_fill_rect(w->x + w->w - 44, w->y + 4, 18, 16, mining ? gfx_mix(0x000000, t->accent, 120) : 0x000000);
    gfx_draw_text(w->x + w->w - 21, w->y + 4, "×", 0xFFFFFF, 1);
    gfx_draw_text(w->x + w->w - 43, w->y + 2, "_", 0xFFFFFF, 1);
    if (w->type == WIN_TERM) draw_term_content(w, focused);
    else if (w->type == WIN_BROWSER) draw_browser_content(w, focused);
    else if (w->type == WIN_CALC) draw_calc_content(w, focused);
    else if (w->type == WIN_GAME) draw_game_content(w);
    else if (w->type == WIN_FILES) draw_files_content(w);
    else if (w->type == WIN_TASKMGR) draw_taskmgr_content(w);
    else draw_about_content(w);
}

/* ширина кнопки окна на панели: сжимается, когда окон много, чтобы не залезать на часы и язык */
static int task_btn_w(void) {
    int n = 0;
    for (int i = 0; i < MAX_WIN; i++) if (wins[i].used) n++;
    if (n == 0) return TASKBTN_W;
    int avail = dw - START_W - 4 - 150;
    int w = avail / n - 4;
    return w > TASKBTN_W ? TASKBTN_W : (w < 40 ? 40 : w);
}

static void draw_taskbar(int focus) {
    const struct theme *t = T();
    int y = dh - TASKBAR_H;
    gfx_fill_rect(0, y, dw, TASKBAR_H, t->taskbar);
    gfx_fill_rect(0, y, dw, 2, t->accent);
    for (int dx = 0; dx < dw; dx += 24) gfx_fill_rect(dx, y + 2, 12, 2, t->accent2);
    gfx_fill_rect(4, y + 4, START_W - 8, TASKBAR_H - 8, start_open ? gfx_mix(t->accent, 0xFFFFFF, 60) : t->accent);
    gfx_draw_text(4 + (START_W - 8 - gfx_text_width("Лимон", 1)) / 2, y + 8, "Лимон", t->on_accent, 1);
    gfx_draw_text(4 + (START_W - 8 - gfx_text_width("Лимон", 1)) / 2 + 1, y + 8, "Лимон", t->on_accent, 1);

    int n = 0;
    for (int i = 0; i < MAX_WIN; i++) {
        if (!wins[i].used) continue;
        int tbw = task_btn_w();
        int bx = START_W + 4 + n * (tbw + 4);
        uint32_t col = (i == focus) ? gfx_mix(t->taskbar, t->accent, 110) : wins[i].minimized ? t->taskbar : t->title_off;
        gfx_fill_rect(bx, y + 4, tbw, TASKBAR_H - 8, col);
        if (i == focus) gfx_fill_rect(bx, y + TASKBAR_H - 6, tbw, 2, t->accent);
        char label[40];
        int len = 0, chars = 0, maxc = (tbw - 12) / FONT_W;
        const char *p = wins[i].title;
        while (*p && chars < maxc && len < 28) { chars++; uint32_t cp = utf8_next(&p); len += utf8_encode(cp, label + len); }
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

int desktop_open_game(int kind);
int desktop_open_calculator(void);
static void menu_action(int item) {
    switch (item) {
    case 0: desktop_open_terminal(); break;
    case 1: desktop_open_browser(NULL); break;
    case 2: desktop_open_calculator(); break;
    case 3: desktop_open_files(-1); break;
    case 4: desktop_open_taskmgr(); break;
    case 5: desktop_open_game(GAME_SNAKE); break;
    case 6: desktop_open_game(GAME_TETRIS); break;
    case 7: desktop_open_game(GAME_PONG); break;
    case 8: desktop_open_game(GAME_2048); break;
    case 9: desktop_open_about(); break;
    case 10: desktop_set_theme((cur_theme + 1) % NTHEMES); break;
    case 11: desktop_power(1); break;
    case 12: desktop_power(0); break;
    default: break;
    }
}

int desktop_open_game(int kind) {
    if (kind < 0 || kind >= GAME_COUNT) return -1;
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].used && wins[i].type == WIN_GAME && wins[i].game->kind == kind) { focus_window(i); redraw = 1; return i; }
    int slot = -1;
    for (int i = 0; i < MAX_GAMES; i++) if (!game_used[i]) { slot = i; break; }
    if (slot < 0) return -1;
    int gw, gh;
    game_size(kind, &gw, &gh);
    int wi = alloc_window(WIN_GAME, gw + 2, TITLE_H + gh + 1, game_title(kind));
    if (wi < 0) return -1;
    game_used[slot] = 1;
    struct game *g = &games[slot];
    uint8_t *raw = (uint8_t *)g;
    for (size_t i = 0; i < sizeof *g; i++) raw[i] = 0;
    for (int k = 0; k < GAME_COUNT; k++) g->best[k] = best_store[k];
    game_init(g, kind, (uint32_t)(timer_ticks * 2654435761u) ^ ((uint32_t)now.sec << 16) ^ (uint32_t)now.min, timer_ticks);
    wins[wi].game = g;
    wins[wi].slot = slot;
    focus_window(wi);
    redraw = 1;
    return wi;
}

int desktop_open_calculator(void) {
    int slot = -1;
    for (int i = 0; i < MAX_CALCS; i++) if (!calc_used[i]) { slot = i; break; }
    if (slot < 0) return -1;
    calc_init(&calcs[slot]);
    calc_used[slot] = 1;
    int wi = alloc_window(WIN_CALC, CALC_W, TITLE_H + CALC_H, "Лимонка");
    if (wi < 0) { calc_used[slot] = 0; return -1; }
    wins[wi].calc = &calcs[slot];
    wins[wi].slot = slot;
    focus_window(wi);
    redraw = 1;
    return wi;
}

#define FILES_W 460
#define FILES_H 360
#define FILES_ROW 20
int desktop_open_files(int start_dir) {
    for (int i = 0; i < MAX_WIN; i++)
        if (wins[i].used && wins[i].type == WIN_FILES) { if (start_dir >= 0) wins[i].files_dir = start_dir; focus_window(i); redraw = 1; return i; }
    int slot = -1;
    for (int i = 0; i < MAX_FILEWINS; i++) if (!fileswin_used[i]) { slot = i; break; }
    if (slot < 0) return -1;
    int wi = alloc_window(WIN_FILES, FILES_W, TITLE_H + FILES_H, "Проводник");
    if (wi < 0) return -1;
    fileswin_used[slot] = 1;
    wins[wi].slot = slot;
    wins[wi].files_dir = start_dir >= 0 ? start_dir : VFS_ROOT;
    wins[wi].files_sel = -1;
    focus_window(wi);
    redraw = 1;
    return wi;
}

#define TASKMGR_W 420
#define TASKMGR_H 340
int desktop_open_taskmgr(void) {
    for (int i = 0; i < MAX_WIN; i++) if (wins[i].used && wins[i].type == WIN_TASKMGR) { focus_window(i); redraw = 1; return i; }
    if (taskmgr_open) return -1;
    int wi = alloc_window(WIN_TASKMGR, TASKMGR_W, TITLE_H + TASKMGR_H, "Диспетчер задач");
    if (wi < 0) return -1;
    taskmgr_open = 1;
    wins[wi].files_sel = -1;
    focus_window(wi);
    redraw = 1;
    return wi;
}

static void open_icon(int i) {
    switch (i) {
    case 0: desktop_open_terminal(); break;
    case 1: desktop_open_about(); break;
    case 2: desktop_open_calculator(); break;
    case 3: desktop_open_browser(NULL); break;
    case 8: desktop_open_files(-1); break;
    case 9: desktop_open_taskmgr(); break;
    default: desktop_open_game(i - 4); break;       /* 4..7 — Змейка, Тетрис, Понг, 2048 */
    }
}

/* ---------- управление браузером ---------- */

static void str_append_cp(char *s, size_t max, uint32_t cp) {
    size_t l = strlen(s);
    if (l + 5 >= max) return;
    int n = utf8_encode(cp, s + l);
    s[l + (size_t)n] = 0;
}
static void str_backspace(char *s) {
    size_t l = strlen(s);
    while (l > 0) { l--; if (((unsigned char)s[l] & 0xC0) != 0x80) break; }
    s[l] = 0;
}

static void browser_submit_form(struct window *w, struct browser *b, int form, int clicked) {
    char url[700];
    if (page_form_url(&b->pg, form, clicked, url, sizeof url) == 0) browser_load(w, b, url, 1);
}

static void browser_back_fwd(struct window *w, struct browser *b, int dir) {
    int np = b->hist_pos + dir;
    if (np < 0 || np >= b->hist_n) return;
    char url[400]; memcpy(url, b->hist[np], sizeof url);
    if (browser_load(w, b, url, 0) == 0) b->hist_pos = np;
}

static void browser_key(struct window *w, const struct key_event *ev) {
    struct browser *b = w->br;
    uint32_t cp = ev->cp;
    int printable = cp >= 32 && cp < 0x110000 && !(ev->mods & (MOD_CTRL | MOD_ALT));
    if (b->addr_focus) {
        if (cp == '\n') { b->addr_focus = 0; char t[640]; ksnprintf(t, sizeof t, "%s", b->addr); browser_go_typed(w, b, t); }
        else if (cp == 27) b->addr_focus = 0;
        else if (cp == '\b') str_backspace(b->addr);
        else if ((ev->mods & MOD_CTRL) && (cp == 'u' || cp == 'U')) b->addr[0] = 0;
        else if (printable) str_append_cp(b->addr, sizeof b->addr, cp);
        return;
    }
    if (b->focus_input >= 0 && b->focus_input < b->pg.ninputs) {
        struct pg_input *in = &b->pg.inputs[b->focus_input];
        if (cp == '\n') { int f = in->form; b->focus_input = -1; browser_submit_form(w, b, f, -1); }
        else if (cp == 27) b->focus_input = -1;
        else if (cp == '\b') str_backspace(in->value);
        else if (cp == '\t') {                                   /* следующее поле */
            for (int k = 1; k <= b->pg.ninputs; k++) {
                int j = (b->focus_input + k) % b->pg.ninputs;
                if (b->pg.inputs[j].kind == IN_TEXT) { b->focus_input = j; break; }
            }
        }
        else if (printable) str_append_cp(in->value, sizeof in->value, cp);
        return;
    }
    int visible = BR_ROWS, maxs = b->pg.nlines - visible > 0 ? b->pg.nlines - visible : 0;
    if ((ev->mods & MOD_CTRL) && (cp == 'l' || cp == 'L' || cp == 0x434 /* д */)) { b->addr_focus = 1; ksnprintf(b->addr, sizeof b->addr, "%s", b->cur); return; }
    if (cp == KEY_F(6)) { b->addr_focus = 1; ksnprintf(b->addr, sizeof b->addr, "%s", b->cur); return; }
    if ((ev->mods & MOD_CTRL) && (cp == 'r' || cp == 'R')) { if (b->cur[0]) { char u[640]; ksnprintf(u, sizeof u, "%s", b->cur); browser_load(w, b, u, 0); } return; }
    if ((ev->mods & MOD_ALT) && cp == KEY_LEFT) { browser_back_fwd(w, b, -1); return; }
    if ((ev->mods & MOD_ALT) && cp == KEY_RIGHT) { browser_back_fwd(w, b, +1); return; }
    if (cp == '\b') { browser_back_fwd(w, b, -1); return; }
    if (cp == KEY_UP) b->scroll -= 3;
    else if (cp == KEY_DOWN) b->scroll += 3;
    else if (cp == KEY_PGUP) b->scroll -= visible - 1;
    else if (cp == KEY_PGDN || cp == ' ') b->scroll += visible - 1;
    else if (cp == KEY_HOME) b->scroll = 0;
    else if (cp == KEY_END) b->scroll = maxs;
    if (b->scroll < 0) b->scroll = 0;
    if (b->scroll > maxs) b->scroll = maxs;
}

static void browser_click(struct window *w, int px, int py) {
    struct browser *b = w->br;
    int x0 = w->x + 1, ty = w->y + TITLE_H, cy = ty + TOOL_H;
    int ax = x0 + 6 + 3 * 28 + 4, aw = w->w - 2 - (ax - x0) - 8;
    if (py < cy) {                                             /* панель инструментов */
        for (int i = 0; i < 3; i++)
            if (in_rect(px, py, x0 + 6 + i * 28, ty + 4, 24, 22)) {
                if (i == 0) browser_back_fwd(w, b, -1);
                else if (i == 1) browser_back_fwd(w, b, +1);
                else if (b->cur[0]) { char u[640]; ksnprintf(u, sizeof u, "%s", b->cur); browser_load(w, b, u, 0); }
                return;
            }
        if (in_rect(px, py, ax, ty + 4, aw, 22)) { b->addr_focus = 1; b->focus_input = -1; ksnprintf(b->addr, sizeof b->addr, "%s", b->cur); }
        return;
    }
    if (py >= cy + BR_ROWS * FONT_H + 1) return;               /* строка состояния */
    int visible = BR_ROWS, maxs = b->pg.nlines - visible > 0 ? b->pg.nlines - visible : 0;
    if (px >= x0 + w->w - 2 - SB_W) {                          /* полоса прокрутки */
        b->scroll += (py < cy + BR_ROWS * FONT_H / 2) ? -(visible - 1) : (visible - 1);
        if (b->scroll < 0) b->scroll = 0;
        if (b->scroll > maxs) b->scroll = maxs;
        return;
    }
    b->addr_focus = 0;
    for (int i = 0; i < b->nhits; i++) {
        struct hit *h = &b->hits[i];
        if (!in_rect(px, py, h->x, h->y, h->w, h->h)) continue;
        if (h->kind == 0) { char u[640]; ksnprintf(u, sizeof u, "%s", b->pg.links[h->idx]); b->focus_input = -1; browser_load(w, b, u, 1); }
        else if (h->kind == 1) b->focus_input = h->idx;
        else { b->focus_input = -1; browser_submit_form(w, b, b->pg.inputs[h->idx].form, h->idx); }
        return;
    }
    b->focus_input = -1;
}

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
            int tbw = task_btn_w();
            int bx = START_W + 4 + n * (tbw + 4);
            if (in_rect(px, py, bx, dh - TASKBAR_H + 4, tbw, TASKBAR_H - 8)) {
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
        else if (w->type == WIN_BROWSER) browser_click(w, px, py);
        else if (w->type == WIN_CALC) {
            for (int r = 0; r < CALC_ROWS; r++)
                for (int c = 0; c < CALC_COLS; c++) {
                    int bx, by, bw, bh; calc_btn_rect(w, r, c, &bx, &by, &bw, &bh);
                    if (in_rect(px, py, bx, by, bw, bh)) { calc_press(w->calc, calc_layout[r][c].cmd); redraw = 1; }
                }
        }
        else if (w->type == WIN_FILES) { files_click(w, px, py); redraw = 1; }
        else if (w->type == WIN_TASKMGR) { taskmgr_click(w, px, py); redraw = 1; }
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
    {   /* Понг: ракетка следует за мышью, пока курсор над окном игры */
        int tw = top_window();
        if (tw >= 0 && drag_win < 0 && wins[tw].type == WIN_GAME && wins[tw].game->kind == GAME_PONG &&
            in_rect(mx, my, wins[tw].x, wins[tw].y + TITLE_H, wins[tw].w, wins[tw].h - TITLE_H)) {
            if (wins[tw].game->paused && !wins[tw].game->over && (e->buttons & 1) == 0) { /* ждём клавишу */ }
            else game_mouse_y(wins[tw].game, my - (wins[tw].y + TITLE_H + 30));
        }
    }
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
    if (f < 0) return;
    if (wins[f].type == WIN_TERM) { shell_key(wins[f].sh, ev); return; }
    if (wins[f].type == WIN_BROWSER) { browser_key(&wins[f], ev); return; }
    if (wins[f].type == WIN_GAME) { game_key(wins[f].game, ev->cp, ev->mods, timer_ticks); redraw = 1; return; }
    if (wins[f].type == WIN_FILES) { files_key(&wins[f], ev); redraw = 1; return; }
    if (wins[f].type == WIN_CALC) {
        struct calc_state *s = wins[f].calc;
        uint32_t cp = ev->cp;
        if (cp >= '0' && cp <= '9') calc_press(s, (char)cp);
        else if (cp == '.' || cp == ',') calc_press(s, '.');
        else if (cp == '+' || cp == '-' || cp == '*' || cp == '/') calc_press(s, (char)cp);
        else if (cp == '\n' || cp == '=') calc_press(s, '=');
        else if (cp == '\b' || cp == KEY_DELETE) calc_press(s, 'B');
        else if (cp == 27) calc_press(s, 'C');
        else if (cp == '%') calc_press(s, '%');
        redraw = 1;
        return;
    }
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

    int top = top_window();
    for (int i = 0; i < MAX_WIN; i++) {                      /* игры */
        if (!wins[i].used || wins[i].type != WIN_GAME) continue;
        struct game *g = wins[i].game;
        if (wins[i].minimized || i != top) {                 /* окно ушло на задний план — пауза, чтобы не проиграть, пока ты в терминале */
            if (!g->over && !g->paused) { g->paused = 1; redraw = 1; }
            continue;
        }
        if (game_tick(g, timer_ticks)) game_pending = 1;
    }
    if (game_pending && timer_ticks - last_game_draw >= 3) {    /* не чаще ~33 кадров в секунду: старому ПК хватит */
        redraw = 1; last_game_draw = timer_ticks; game_pending = 0;
    }
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
