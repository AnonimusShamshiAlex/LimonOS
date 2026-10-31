#include "browser.h"
#include "mem.h"
#include "kstring.h"
#include "net.h"
#include "format.h"

#define LINEMAX 1100
#define WORDMAX 160
#define MAX_DEPTH 120

/* ================= общие помощники ================= */

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static size_t ci_find(const char *h, size_t n, size_t from, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0 || from + nl > n) return n;
    for (size_t i = from; i + nl <= n; i++) {
        size_t j = 0;
        while (j < nl && lc(h[i + j]) == needle[j]) j++;
        if (j == nl) return i;
    }
    return n;
}

static void copy_str(char *dst, size_t max, const char *src) {
    size_t i = 0; while (src[i] && i + 1 < max) { dst[i] = src[i]; i++; } dst[i] = 0;
}
static char *dup_n(const char *s, size_t n) {
    char *p = kmalloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n); p[n] = 0;
    return p;
}

struct ent { const char *name; uint32_t cp; };
static const struct ent entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "laquo", 0xAB }, { "raquo", 0xBB },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "middot", 0xB7 }, { "bull", 0x2022 },
    { "rarr", 0x2192 }, { "larr", 0x2190 }, { "euro", 0x20AC }, { "pound", 0xA3 }, { "yen", 0xA5 },
    { "times", 0xD7 }, { "deg", 0xB0 }, { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "lsquo", 0x2018 },
    { "rsquo", 0x2019 }, { "sect", 0xA7 }, { "para", 0xB6 },
};

/* &...; в позиции i. Возвращает сколько байт съели (0 — не сущность) */
static size_t decode_entity(const char *h, size_t n, size_t i, uint32_t *cp) {
    if (h[i] != '&' || i + 2 >= n) return 0;
    size_t k = i + 1;
    if (h[k] == '#') {
        k++;
        int hex = (k < n && (h[k] == 'x' || h[k] == 'X')); if (hex) k++;
        uint32_t v = 0; size_t st = k;
        while (k < n && k - st < 8) {
            char c = h[k]; int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && lc(c) >= 'a' && lc(c) <= 'f') d = lc(c) - 'a' + 10;
            else break;
            v = v * (hex ? 16u : 10u) + (uint32_t)d; k++;
        }
        if (k == st) return 0;
        if (k < n && h[k] == ';') k++;
        *cp = v ? v : '?';
        return k - i;
    }
    size_t e = k; while (e < n && e - k < 10 && h[e] != ';' && h[e] != ' ' && h[e] != '<' && h[e] != '&') e++;
    if (e >= n || h[e] != ';') return 0;
    for (unsigned j = 0; j < sizeof entities / sizeof entities[0]; j++)
        if (strlen(entities[j].name) == e - k && strncmp(entities[j].name, h + k, e - k) == 0) { *cp = entities[j].cp; return e + 1 - i; }
    return 0;
}

/* Шрифт 8x16 знает ASCII, кириллицу и немного знаков. Остальное подменяем похожим. 0 — выбросить. */
static uint32_t cp_fallback(uint32_t cp) {
    static const char l1[] = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    static const char ex[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
    if (cp < 0x80) return cp;
    if (cp == 0xA0) return ' ';
    if (cp >= 0xC0 && cp <= 0xFF) return (uint32_t)l1[cp - 0xC0];
    if (cp >= 0x100 && cp <= 0x17F) return (uint32_t)ex[cp - 0x100];
    switch (cp) {
    case 0x2BB: case 0x2BC: case 0x2B9: case 0x2018: case 0x2019: case 0x2032: case 0x60: return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2212: return '-';
    case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: case 0xAD: case 0x200E: case 0x200F: return 0;
    case 0x2002: case 0x2003: case 0x2009: case 0x202F: case 0x3000: return ' ';
    case 0x20AC: return 'E';
    case 0x2122: return 'T';
    case 0xA9: return 'c';
    case 0x456: return 'i'; case 0x406: return 'I'; case 0x457: return 'i'; case 0x407: return 'I';
    case 0x454: return 'e'; case 0x404: return 'E'; case 0x491: return 0x433; case 0x490: return 0x413;
    case 0x45E: return 0x443; case 0x40E: return 0x423;
    case 0x49B: return 0x43A; case 0x49A: return 0x41A; case 0x493: return 0x433; case 0x492: return 0x413;
    case 0x4B3: return 0x445; case 0x4B2: return 0x425; case 0x4A3: return 0x43D; case 0x4A2: return 0x41D;
    case 0x4AF: return 0x443; case 0x4AE: return 0x423; case 0x4E9: return 0x43E; case 0x4E8: return 0x41E;
    case 0x4D9: return 0x430; case 0x4D8: return 0x410; case 0x4BB: return 'h'; case 0x4BA: return 'H';
    }
    return cp;
}

/* ================= DOM ================= */

struct node {
    uint8_t kind;                 /* 0 — элемент, 1 — текст */
    char tag[12];
    char *text;                   /* текст; у <style>/<script>/<title>/<textarea> — их содержимое */
    char *attrs;                  /* "имя атрибуты..." — как в исходном теге */
    size_t attrs_len;
    char *id, *cls, *style;
    uint8_t hidden;
    struct node *parent, *first, *last, *next;
};

static const char void_tags[] = "br img input meta link hr base area col embed source track wbr param ";
static const char raw_tags[]  = "script style textarea title ";

static int tag_in(const char *name, const char *list) {          /* list: "a b c " с пробелом в конце */
    size_t nl = strlen(name);
    if (!nl) return 0;
    for (const char *p = list; *p; ) {
        const char *e = p; while (*e && *e != ' ') e++;
        if ((size_t)(e - p) == nl && strncmp(p, name, nl) == 0) return 1;
        p = e; while (*p == ' ') p++;
    }
    return 0;
}

static int get_attr(const char *t, size_t n, const char *name, char *out, size_t max) {
    size_t i = 0;
    while (i < n && t[i] != ' ' && t[i] != '\t' && t[i] != '\n' && t[i] != '\r' && t[i] != '/') i++;   /* имя тега */
    size_t nl = strlen(name);
    while (i < n) {
        while (i < n && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r' || t[i] == '/')) i++;
        size_t s = i;
        while (i < n && t[i] != '=' && t[i] != ' ' && t[i] != '\t' && t[i] != '\n' && t[i] != '\r' && t[i] != '/') i++;
        size_t ne = i;
        while (i < n && (t[i] == ' ' || t[i] == '\t')) i++;
        char q = 0; size_t vs = i, ve = i;
        if (i < n && t[i] == '=') {
            i++;
            while (i < n && (t[i] == ' ' || t[i] == '\t')) i++;
            if (i < n && (t[i] == '"' || t[i] == '\'')) { q = t[i++]; vs = i; while (i < n && t[i] != q) i++; ve = i; if (i < n) i++; }
            else { vs = i; while (i < n && t[i] != ' ' && t[i] != '\t' && t[i] != '\n' && t[i] != '\r') i++; ve = i; }
        } else { vs = ve = i; }
        if (ne - s == nl) {
            size_t j = 0; while (j < nl && lc(t[s + j]) == name[j]) j++;
            if (j == nl) {
                size_t o = 0;
                for (size_t k = vs; k < ve && o + 1 < max; ) {
                    uint32_t cp; size_t e = decode_entity(t, ve, k, &cp);
                    if (e && cp < 0x80) { out[o++] = (char)cp; k += e; }
                    else out[o++] = t[k++];
                }
                out[o] = 0;
                return 1;
            }
        }
    }
    return 0;
}

static struct node *new_node(int kind) {
    struct node *n = kmalloc(sizeof *n);
    if (n) memset(n, 0, sizeof *n);
    if (n) n->kind = (uint8_t)kind;
    return n;
}
static void append_child(struct node *p, struct node *c) {
    c->parent = p;
    if (p->last) p->last->next = c; else p->first = c;
    p->last = c;
}
static void free_tree(struct node *n) {
    while (n) {
        struct node *nx = n->next;
        free_tree(n->first);
        if (n->text) kfree(n->text);
        if (n->attrs) kfree(n->attrs);
        if (n->id) kfree(n->id);
        if (n->cls) kfree(n->cls);
        if (n->style) kfree(n->style);
        kfree(n);
        n = nx;
    }
}

static void add_text(struct node *parent, const char *s, size_t n) {
    if (!n) return;
    struct node *t = new_node(1);
    if (!t) return;
    t->text = dup_n(s, n);
    if (!t->text) { kfree(t); return; }
    append_child(parent, t);
}

/* HTML -> дерево. Закрывающие теги ищут подходящий открытый предок; <p>, <li>, <td> и т.п. закрываются сами. */
static struct node *dom_build(const char *h, size_t n) {
    struct node *root = new_node(0);
    if (!root) return NULL;
    copy_str(root->tag, sizeof root->tag, "#root");
    struct node *stack[MAX_DEPTH + 2];
    int sp = 0;
    stack[0] = root;
    size_t i = 0;
    char *tb = kmalloc(8192);
    if (!tb) return root;
    size_t tn = 0;
    while (i < n) {
        if (h[i] == '<' && i + 1 < n) {
            char c1 = h[i + 1];
            if (c1 == '!' || c1 == '?') {
                if (i + 3 < n && h[i + 2] == '-' && h[i + 3] == '-') { size_t e = ci_find(h, n, i + 4, "-->"); i = e < n ? e + 3 : n; }
                else { while (i < n && h[i] != '>') i++; i++; }
                continue;
            }
            int closing = (c1 == '/');
            size_t ns = i + 1 + (closing ? 1 : 0);
            if (ns < n && lc(h[ns]) >= 'a' && lc(h[ns]) <= 'z') {
                if (tn) { add_text(stack[sp], tb, tn); tn = 0; }
                size_t j = ns; char q = 0;
                while (j < n && (q || h[j] != '>')) { if (q) { if (h[j] == q) q = 0; } else if (h[j] == '"' || h[j] == '\'') q = h[j]; j++; }
                char name[12]; size_t k = 0;
                while (ns + k < j && k < 11 && ((lc(h[ns + k]) >= 'a' && lc(h[ns + k]) <= 'z') || (h[ns + k] >= '0' && h[ns + k] <= '9'))) { name[k] = lc(h[ns + k]); k++; }
                name[k] = 0;
                size_t pos = j < n ? j + 1 : n;
                if (closing) {
                    for (int s = sp; s > 0; s--)
                        if (strcmp(stack[s]->tag, name) == 0) { sp = s - 1; break; }
                } else {
                    /* неявные закрытия */
                    if (tag_in(name, "p div ul ol dl table h1 h2 h3 h4 h5 h6 pre form hr blockquote fieldset address section article header footer nav ") &&
                        strcmp(stack[sp]->tag, "p") == 0 && sp > 0) sp--;
                    if (strcmp(name, "li") == 0) {
                        for (int s = sp; s > 0; s--) {
                            if (tag_in(stack[s]->tag, "ul ol menu ")) break;
                            if (strcmp(stack[s]->tag, "li") == 0) { sp = s - 1; break; }
                        }
                    } else if (tag_in(name, "dt dd")) {
                        for (int s = sp; s > 0; s--) {
                            if (strcmp(stack[s]->tag, "dl") == 0) break;
                            if (tag_in(stack[s]->tag, "dt dd ")) { sp = s - 1; break; }
                        }
                    } else if (tag_in(name, "td th")) {
                        for (int s = sp; s > 0; s--) {
                            if (strcmp(stack[s]->tag, "tr") == 0 || strcmp(stack[s]->tag, "table") == 0) break;
                            if (tag_in(stack[s]->tag, "td th ")) { sp = s - 1; break; }
                        }
                    } else if (strcmp(name, "tr") == 0) {
                        for (int s = sp; s > 0; s--) {
                            if (strcmp(stack[s]->tag, "table") == 0) break;
                            if (strcmp(stack[s]->tag, "tr") == 0) { sp = s - 1; break; }
                        }
                    }
                    struct node *el = new_node(0);
                    if (el) {
                        copy_str(el->tag, sizeof el->tag, name);
                        size_t tl = j - ns;
                        el->attrs = dup_n(h + ns, tl); el->attrs_len = tl;
                        char v[400];
                        if (el->attrs) {
                            if (get_attr(el->attrs, tl, "id", v, sizeof v)) el->id = dup_n(v, strlen(v));
                            if (get_attr(el->attrs, tl, "class", v, sizeof v)) el->cls = dup_n(v, strlen(v));
                            if (get_attr(el->attrs, tl, "style", v, sizeof v)) el->style = dup_n(v, strlen(v));
                            char hv[8]; if (get_attr(el->attrs, tl, "hidden", hv, sizeof hv)) el->hidden = 1;
                        }
                        append_child(stack[sp], el);
                        int self_closed = (j > ns && h[j - 1] == '/');
                        if (tag_in(name, raw_tags)) {
                            char close[16]; ksnprintf(close, sizeof close, "</%s", name);
                            size_t c = ci_find(h, n, pos, close);
                            if (c > pos) add_text(el, h + pos, c - pos);
                            size_t gt = c; while (gt < n && h[gt] != '>') gt++;
                            pos = gt < n ? gt + 1 : n;
                        } else if (!tag_in(name, void_tags) && !self_closed && sp < MAX_DEPTH) {
                            stack[++sp] = el;
                        }
                    }
                }
                i = pos;
                continue;
            }
        }
        if (h[i] == '&') {
            uint32_t cp; size_t e = decode_entity(h, n, i, &cp);
            if (e) { if (tn + 5 >= 8192) { add_text(stack[sp], tb, tn); tn = 0; } tn += (size_t)utf8_encode(cp, tb + tn); i += e; continue; }
        }
        if (tn + 2 >= 8192) { add_text(stack[sp], tb, tn); tn = 0; }
        tb[tn++] = h[i++];
    }
    if (tn) add_text(stack[sp], tb, tn);
    kfree(tb);
    return root;
}

/* ================= CSS ================= */

#define CSS_MAXRULES 700
struct cmp { char tag[12]; char id[24]; char cls[2][24]; uint8_t ncls; uint8_t child; };   /* child: связь с ПРЕДЫДУЩИМ компонентом: '>' */
struct css_rule { struct cmp c[4]; uint8_t n; uint16_t spec; char *decls; };
struct css { struct css_rule *r; int n; };

static int parse_selector(const char *s, size_t n, struct css_rule *r) {
    memset(r, 0, sizeof *r);
    size_t i = 0;
    int spec = 0, child_next = 0;
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
        if (i >= n) break;
        if (s[i] == '>') { child_next = 1; i++; continue; }
        if (s[i] == '+' || s[i] == '~') return -1;
        if (r->n >= 4) return -1;
        struct cmp *c = &r->c[r->n];
        c->child = (uint8_t)child_next; child_next = 0;
        size_t any = 0;
        while (i < n && s[i] != ' ' && s[i] != '>' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') {
            char ch = s[i];
            if (ch == '*') { i++; any++; continue; }
            if (ch == '#' || ch == '.') {
                size_t st = ++i;
                while (i < n && (s[i] == '-' || s[i] == '_' || (s[i] >= '0' && s[i] <= '9') || (lc(s[i]) >= 'a' && lc(s[i]) <= 'z') || (unsigned char)s[i] >= 0x80)) i++;
                size_t l = i - st; if (!l || l > 23) return -1;
                if (ch == '#') { memcpy(c->id, s + st, l); c->id[l] = 0; spec += 100; }
                else { if (c->ncls >= 2) return -1; memcpy(c->cls[c->ncls], s + st, l); c->cls[c->ncls][l] = 0; c->ncls++; spec += 10; }
                any++; continue;
            }
            if ((lc(ch) >= 'a' && lc(ch) <= 'z') || (ch >= '0' && ch <= '9')) {
                size_t st = i;
                while (i < n && ((lc(s[i]) >= 'a' && lc(s[i]) <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) i++;
                size_t l = i - st; if (l > 11) return -1;
                for (size_t k = 0; k < l; k++) c->tag[k] = lc(s[st + k]);
                c->tag[l] = 0; spec += 1; any++; continue;
            }
            return -1;                                       /* [attr], :псевдо, ::псевдо и прочее — не поддерживаем: правило отбрасываем */
        }
        if (!any) return -1;
        r->n++;
    }
    if (!r->n) return -1;
    r->spec = (uint16_t)spec;
    return 0;
}

static void css_parse(struct css *css, const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') { size_t e = ci_find(s, n, i + 2, "*/"); i = e < n ? e + 2 : n; continue; }
        if (i + 3 < n && s[i] == '<' && s[i + 1] == '!') { i += 4; continue; }
        if (i + 2 < n && s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>') { i += 3; continue; }
        if (i >= n) break;
        size_t ps = i;
        while (i < n && s[i] != '{' && s[i] != ';') i++;
        if (i >= n) break;
        if (s[i] == ';') { i++; continue; }                          /* @import …; */
        size_t pe = i;
        size_t bs = ++i; int depth = 1;
        while (i < n && depth) { if (s[i] == '{') depth++; else if (s[i] == '}') depth--; i++; }
        size_t be = depth ? n : i - 1;
        if (s[ps] == '@') continue;                                  /* @media/@font-face/@keyframes — пропускаем целиком */
        /* список селекторов через запятую */
        size_t a = ps;
        for (size_t k = ps; k <= pe; k++) {
            if (k == pe || s[k] == ',') {
                if (css->n < CSS_MAXRULES) {
                    struct css_rule *r = &css->r[css->n];
                    if (parse_selector(s + a, k - a, r) == 0) {
                        r->decls = dup_n(s + bs, be - bs);
                        if (r->decls) css->n++;
                    }
                }
                a = k + 1;
            }
        }
    }
}

struct cstyle {
    int8_t display, bold, align, pre;           /* -1 — не задано; display: 0 none, 1 block, 2 inline */
    uint32_t color, bg;                         /* 0 — не задано; иначе 0x1000000 | RRGGBB */
    uint16_t sp_display, sp_bold, sp_align, sp_pre, sp_color, sp_bg;
};

static const struct { const char *n; uint32_t c; } named_colors[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 }, { "blue", 0x0000FF },
    { "yellow", 0xFFFF00 }, { "orange", 0xFFA500 }, { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xC0C0C0 },
    { "maroon", 0x800000 }, { "navy", 0x000080 }, { "purple", 0x800080 }, { "teal", 0x008080 }, { "lime", 0x00FF00 },
    { "aqua", 0x00FFFF }, { "cyan", 0x00FFFF }, { "fuchsia", 0xFF00FF }, { "magenta", 0xFF00FF }, { "olive", 0x808000 },
    { "brown", 0xA52A2A }, { "pink", 0xFFC0CB }, { "gold", 0xFFD700 }, { "darkgray", 0xA9A9A9 }, { "lightgray", 0xD3D3D3 },
    { "darkblue", 0x00008B }, { "darkgreen", 0x006400 }, { "darkred", 0x8B0000 }, { "lightblue", 0xADD8E6 },
    { "crimson", 0xDC143C }, { "coral", 0xFF7F50 }, { "tomato", 0xFF6347 }, { "royalblue", 0x4169E1 },
};

static int hexv(char c) { if (c >= '0' && c <= '9') return c - '0'; c = lc(c); if (c >= 'a' && c <= 'f') return c - 'a' + 10; return -1; }

static int parse_color_tok(const char *v, uint32_t *out) {
    while (*v == ' ') v++;
    if (*v == '#') {
        int d[6], k = 0; v++;
        while (k < 6 && hexv(v[k]) >= 0) { d[k] = hexv(v[k]); k++; }
        if (k == 3 || k == 4) { *out = (uint32_t)(((d[0] * 17) << 16) | ((d[1] * 17) << 8) | (d[2] * 17)); return 1; }
        if (k >= 6) { *out = (uint32_t)((d[0] << 20) | (d[1] << 16) | (d[2] << 12) | (d[3] << 8) | (d[4] << 4) | d[5]); return 1; }
        return 0;
    }
    if (strncmp(v, "rgb", 3) == 0) {
        const char *p = v; while (*p && *p != '(') p++;
        if (!*p) return 0;
        p++;
        int comp[4] = { 0, 0, 0, 255 }, nc = 0;
        while (*p && *p != ')' && nc < 4) {
            while (*p == ' ' || *p == ',' || *p == '/') p++;
            int val = 0, any = 0, frac = 0;
            while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; any = 1; }
            if (*p == '.') { p++; frac = 1; while (*p >= '0' && *p <= '9') p++; }
            if (*p == '%') { val = val * 255 / 100; p++; }
            if (nc == 3 && frac && val == 0) val = 0; else if (nc == 3 && frac) val = 255;
            if (!any) break;
            comp[nc++] = val > 255 ? 255 : val;
        }
        if (nc < 3 || comp[3] == 0) return 0;
        *out = (uint32_t)((comp[0] << 16) | (comp[1] << 8) | comp[2]);
        return 1;
    }
    for (unsigned i = 0; i < sizeof named_colors / sizeof named_colors[0]; i++) {
        size_t l = strlen(named_colors[i].n); size_t j = 0;
        while (j < l && lc(v[j]) == named_colors[i].n[j]) j++;
        if (j == l && !((lc(v[j]) >= 'a' && lc(v[j]) <= 'z'))) { *out = named_colors[i].c; return 1; }
    }
    return 0;
}

/* ищет цвет в значении (для background: shorthand) */
static int find_color(const char *v, uint32_t *out) {
    for (const char *p = v; *p; p++) {
        if ((p == v || p[-1] == ' ') && parse_color_tok(p, out)) return 1;
    }
    return 0;
}

static void apply_decl(struct cstyle *cs, const char *prop, const char *val, uint16_t sp) {
    uint32_t col;
    if (strcmp(prop, "display") == 0) {
        if (sp < cs->sp_display) return;
        int8_t d = -1;
        if (strncmp(val, "none", 4) == 0) d = 0;
        else if (strncmp(val, "inline", 6) == 0 || strncmp(val, "contents", 8) == 0) d = 2;
        else if (val[0]) d = 1;
        if (d >= 0) { cs->display = d; cs->sp_display = sp; }
    } else if (strcmp(prop, "visibility") == 0) {
        if (sp >= cs->sp_display && (strncmp(val, "hidden", 6) == 0 || strncmp(val, "collapse", 8) == 0)) { cs->display = 0; cs->sp_display = sp; }
    } else if (strcmp(prop, "opacity") == 0) {
        if (sp >= cs->sp_display && val[0] == '0' && (val[1] == 0 || val[1] == ' ' || val[1] == ';')) { cs->display = 0; cs->sp_display = sp; }
    } else if (strcmp(prop, "color") == 0) {
        if (sp >= cs->sp_color && parse_color_tok(val, &col)) { cs->color = 0x1000000u | col; cs->sp_color = sp; }
    } else if (strcmp(prop, "background-color") == 0 || strcmp(prop, "background") == 0) {
        if (sp >= cs->sp_bg && find_color(val, &col)) { cs->bg = 0x1000000u | col; cs->sp_bg = sp; }
    } else if (strcmp(prop, "font-weight") == 0) {
        if (sp < cs->sp_bold) return;
        int num = 0, isnum = 0; for (const char *p = val; *p >= '0' && *p <= '9'; p++) { num = num * 10 + (*p - '0'); isnum = 1; }
        if (strncmp(val, "bold", 4) == 0 || (isnum && num >= 600)) { cs->bold = 1; cs->sp_bold = sp; }
        else if (strncmp(val, "normal", 6) == 0 || strncmp(val, "lighter", 7) == 0 || (isnum && num <= 500)) { cs->bold = 0; cs->sp_bold = sp; }
    } else if (strcmp(prop, "text-align") == 0) {
        if (sp < cs->sp_align) return;
        if (strncmp(val, "center", 6) == 0) { cs->align = 1; cs->sp_align = sp; }
        else if (strncmp(val, "right", 5) == 0 || strncmp(val, "end", 3) == 0) { cs->align = 2; cs->sp_align = sp; }
        else if (strncmp(val, "left", 4) == 0 || strncmp(val, "start", 5) == 0 || strncmp(val, "justify", 7) == 0) { cs->align = 0; cs->sp_align = sp; }
    } else if (strcmp(prop, "white-space") == 0) {
        if (sp < cs->sp_pre) return;
        if (strncmp(val, "pre", 3) == 0) { cs->pre = 1; cs->sp_pre = sp; }
        else if (strncmp(val, "normal", 6) == 0 || strncmp(val, "nowrap", 6) == 0) { cs->pre = 0; cs->sp_pre = sp; }
    }
}

static void apply_decls(struct cstyle *cs, const char *d, uint16_t sp) {
    const char *p = d;
    while (*p) {
        while (*p == ' ' || *p == ';' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        char prop[32]; size_t pl = 0;
        while (*p && *p != ':' && *p != ';' && pl < 31) { prop[pl++] = lc(*p); p++; }
        while (pl && (prop[pl - 1] == ' ' || prop[pl - 1] == '\n' || prop[pl - 1] == '\t')) pl--;
        prop[pl] = 0;
        if (*p != ':') { while (*p && *p != ';') p++; continue; }
        p++;
        while (*p == ' ') p++;
        char val[120]; size_t vl = 0; int depth = 0;
        while (*p && (depth || *p != ';') && vl < 119) { if (*p == '(') depth++; if (*p == ')' && depth) depth--; val[vl++] = (*p == '\n' || *p == '\t') ? ' ' : *p; p++; }
        val[vl] = 0;
        uint16_t spx = sp;
        char *imp = NULL;
        for (size_t k = 0; k + 9 <= vl; k++) if (val[k] == '!') { imp = val + k; break; }
        if (imp) { spx = (uint16_t)(spx + 20000); *imp = 0; }
        for (char *q = val; *q; q++) *q = lc(*q);
        apply_decl(cs, prop, val, spx);
    }
}

static int cmp_match(const struct cmp *c, const struct node *n) {
    if (n->kind != 0) return 0;
    if (c->tag[0] && strcmp(c->tag, n->tag) != 0) return 0;
    if (c->id[0] && (!n->id || strcmp(c->id, n->id) != 0)) return 0;
    for (int k = 0; k < c->ncls; k++) {
        if (!n->cls) return 0;
        size_t cl = strlen(c->cls[k]); const char *p = n->cls; int found = 0;
        while (*p) {
            while (*p == ' ') p++;
            const char *e = p; while (*e && *e != ' ') e++;
            if ((size_t)(e - p) == cl && strncmp(p, c->cls[k], cl) == 0) { found = 1; break; }
            p = e;
        }
        if (!found) return 0;
    }
    return 1;
}

static int sel_match(const struct css_rule *r, int idx, const struct node *n) {
    if (!cmp_match(&r->c[idx], n)) return 0;
    if (idx == 0) return 1;
    const struct node *p = n->parent;
    if (r->c[idx].child) return p && sel_match(r, idx - 1, p);
    for (; p; p = p->parent) if (sel_match(r, idx - 1, p)) return 1;
    return 0;
}

static void compute_style(const struct css *css, const struct node *n, struct cstyle *cs) {
    memset(cs, 0, sizeof *cs);
    cs->display = cs->bold = cs->align = cs->pre = -1;
    char v[100];
    if (n->attrs) {                                           /* HTML-атрибуты оформления (самый низкий приоритет) */
        if (get_attr(n->attrs, n->attrs_len, "align", v, sizeof v)) {
            if (lc(v[0]) == 'c') { cs->align = 1; cs->sp_align = 1; } else if (lc(v[0]) == 'r') { cs->align = 2; cs->sp_align = 1; }
        }
        uint32_t col;
        if (strcmp(n->tag, "font") == 0 && get_attr(n->attrs, n->attrs_len, "color", v, sizeof v) && parse_color_tok(v, &col)) { cs->color = 0x1000000u | col; cs->sp_color = 1; }
        if ((strcmp(n->tag, "body") == 0 || strcmp(n->tag, "table") == 0 || strcmp(n->tag, "td") == 0) &&
            get_attr(n->attrs, n->attrs_len, "bgcolor", v, sizeof v) && parse_color_tok(v, &col)) { cs->bg = 0x1000000u | col; cs->sp_bg = 1; }
    }
    if (strcmp(n->tag, "center") == 0) { cs->align = 1; cs->sp_align = 1; }
    for (int i = 0; i < css->n; i++) {
        const struct css_rule *r = &css->r[i];
        if (sel_match(r, r->n - 1, n)) apply_decls(cs, r->decls, (uint16_t)(2 + r->spec));
    }
    if (n->style) apply_decls(cs, n->style, 1000);
}

static void css_collect(struct css *css, const struct node *n) {
    for (; n; n = n->next) {
        if (n->kind == 0 && strcmp(n->tag, "style") == 0 && n->first && n->first->text) css_parse(css, n->first->text, strlen(n->first->text));
        if (n->first) css_collect(css, n->first);
    }
}
static void css_free(struct css *css) {
    for (int i = 0; i < css->n; i++) kfree(css->r[i].decls);
    kfree(css->r);
    css->r = NULL; css->n = 0;
}

/* ================= вёрстка ================= */

struct fb {
    struct page *pg;
    int cols;
    char line[LINEMAX]; int lb, lcols; int started;
    int indent;
    uint32_t color_cur; int bold_cur;            /* что уже выдано в текущую строку */
    int head, bold, nobold, code, link;
    uint32_t rgb;                                /* явный цвет текста: 0x1000000|RRGGBB, 0 — палитра */
    int align;                                   /* 0 лево, 1 центр, 2 право */
    int pending_space, last_blank, pre;
    int form;
    const char *base;
    char word[WORDMAX]; int wb;
    const struct css *css;
};

static void emit_line(struct fb *f) {
    struct page *pg = f->pg;
    if (f->align && f->lcols > 0 && f->lcols < f->cols && !f->pre) {      /* выравнивание по центру/правому краю */
        int pad = f->align == 1 ? (f->cols - f->lcols) / 2 : (f->cols - f->lcols);
        if (pad > 0 && f->lb + pad < LINEMAX - 4) {
            memmove(f->line + pad, f->line, (size_t)f->lb);
            for (int i = 0; i < pad; i++) f->line[i] = ' ';
            f->lb += pad;
        }
    }
    if (pg->nlines < PG_MAXLINES) {
        char *s = kmalloc((size_t)f->lb + 1);
        if (s) { memcpy(s, f->line, (size_t)f->lb); s[f->lb] = 0; pg->lines[pg->nlines++] = s; }
    }
    f->last_blank = (f->lcols == 0);
    f->lb = 0; f->lcols = 0; f->started = 0; f->color_cur = 0; f->bold_cur = 0;
}

static void put_b(struct fb *f, char c) { if (f->lb < LINEMAX - 4) f->line[f->lb++] = c; }

static uint32_t style_key(const struct fb *f) {
    if (f->rgb) return f->rgb;                   /* уже с флагом 0x1000000: чёрный #000000 тоже цвет */
    if (f->link >= 0) return PG_C_LINK;
    if (f->head) return PG_C_HEAD;
    if (f->code) return PG_C_CODE;
    return PG_C_TEXT;
}
static int bold_eff(const struct fb *f) { return (f->bold && !f->nobold) || f->head; }

static void put_link_marker(struct fb *f) {
    put_b(f, PGM_LINK); put_b(f, (char)('0' + (f->link >> 6))); put_b(f, (char)('0' + (f->link & 63)));
}
static void put_color(struct fb *f, uint32_t key) {
    if (key >= 0x1000000u) {
        static const char hx[] = "0123456789ABCDEF";
        put_b(f, PGM_RGB);
        for (int s = 20; s >= 0; s -= 4) put_b(f, hx[(key >> s) & 15]);
    } else { put_b(f, PGM_COLOR); put_b(f, (char)('0' + key)); }
}

static void begin_line(struct fb *f) {
    if (f->started) return;
    f->started = 1;
    for (int i = 0; i < f->indent && !f->pre; i++) { put_b(f, ' '); f->lcols++; }
    uint32_t k = style_key(f);
    if (k != PG_C_TEXT) put_color(f, k);
    f->color_cur = k;
    if (bold_eff(f)) { put_b(f, PGM_BOLD); put_b(f, '1'); f->bold_cur = 1; }
    if (f->link >= 0) put_link_marker(f);
}

static void update_style(struct fb *f) {
    if (!f->started) return;
    uint32_t k = style_key(f);
    if (k != f->color_cur) { put_color(f, k); f->color_cur = k; }
    int b = bold_eff(f);
    if (b != f->bold_cur) { put_b(f, PGM_BOLD); put_b(f, (char)('0' + b)); f->bold_cur = b; }
}

/* перевод строки; blanks=2 — ещё и пустая строка */
static void br(struct fb *f, int blanks) {
    if (f->lcols > 0 || (f->started && f->lb > 0)) emit_line(f);
    if (blanks >= 2 && !f->last_blank && f->pg->nlines > 0) { f->lcols = 0; f->lb = 0; emit_line(f); }
    f->pending_space = 0;
}

static void hard_newline(struct fb *f) {          /* для <br> и pre: пустые строки сохраняются */
    if (f->lcols == 0 && f->last_blank && !f->pre) return;
    emit_line(f);
}

static void append_raw(struct fb *f, const char *s) {
    begin_line(f);
    while (*s) { const char *p = s; utf8_next(&s); for (; p < s; p++) put_b(f, *p); f->lcols++; }
}

static void put_word(struct fb *f, const char *w, int wcols) {
    int cols = f->cols;
    begin_line(f);
    if (f->lcols > f->indent && f->pending_space) {
        if (f->lcols + 1 + wcols > cols) { emit_line(f); begin_line(f); }
        else { put_b(f, ' '); f->lcols++; }
    } else if (f->lcols > f->indent && f->lcols + wcols > cols && wcols <= cols - f->indent) { emit_line(f); begin_line(f); }
    f->pending_space = 0;
    while (*w) {
        if (f->lcols >= cols) { emit_line(f); begin_line(f); }
        const char *p = w; utf8_next(&w);
        for (; p < w; p++) put_b(f, *p);
        f->lcols++;
    }
}

static void flush_word(struct fb *f) {
    if (!f->wb) return;
    f->word[f->wb] = 0;
    put_word(f, f->word, utf8_len(f->word));
    f->wb = 0;
}
static void word_add_cp(struct fb *f, uint32_t cp) {
    if (f->wb + 5 >= WORDMAX) flush_word(f);
    f->wb += utf8_encode(cp, f->word + f->wb);
}
static void space(struct fb *f) { flush_word(f); if (f->started && f->lcols > f->indent) f->pending_space = 1; }

static void feed_text(struct fb *f, const char *s) {
    const char *p = s;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        uint32_t cp;
        if (c < 0x80) { cp = c; p++; } else { cp = utf8_next(&p); }
        cp = cp_fallback(cp);
        if (!cp) continue;
        if (f->pre) {
            if (cp == '\n') { flush_word(f); emit_line(f); }
            else if (cp == '\r') { }
            else if (cp == '\t' || cp == ' ') {
                begin_line(f);
                int cnt = (cp == '\t') ? 4 : 1;
                while (cnt--) { if (f->lcols >= f->cols) { emit_line(f); begin_line(f); } put_b(f, ' '); f->lcols++; }
            } else if (cp >= 0x20) {
                begin_line(f);
                if (f->lcols >= f->cols) { emit_line(f); begin_line(f); }
                char tmp[5]; int l = utf8_encode(cp, tmp);
                for (int i = 0; i < l; i++) put_b(f, tmp[i]);
                f->lcols++;
            }
            continue;
        }
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') space(f);
        else if (cp >= 0x20) word_add_cp(f, cp);
    }
}

/* ---------- ссылки и формы ---------- */

static int add_link(struct fb *f, const char *href) {
    struct page *pg = f->pg;
    if (!href[0] || href[0] == '#' || strncmp(href, "javascript:", 11) == 0 || strncmp(href, "mailto:", 7) == 0 ||
        strncmp(href, "tel:", 4) == 0 || strncmp(href, "data:", 5) == 0) return -1;
    if (pg->nlinks >= PG_MAXLINKS) return -1;
    char abs[700];
    if (net_url_join(f->base, href, abs, sizeof abs) != 0) return -1;
    char *s = dup_n(abs, strlen(abs));
    if (!s) return -1;
    pg->links[pg->nlinks] = s;
    return pg->nlinks++;
}

static int add_input(struct fb *f, int kind, const char *name, const char *value, int pw) {
    struct page *pg = f->pg;
    if (pg->ninputs >= PG_MAXINPUTS) return -1;
    struct pg_input *in = &pg->inputs[pg->ninputs];
    memset(in, 0, sizeof *in);
    in->kind = (uint8_t)kind; in->password = (uint8_t)pw; in->form = f->form;
    copy_str(in->name, sizeof in->name, name);
    copy_str(in->value, sizeof in->value, value);
    return pg->ninputs++;
}

static void emit_widget(struct fb *f, int marker, int idx, int wcols) {
    begin_line(f);
    if (f->lcols > f->indent && f->lcols + wcols + (f->pending_space ? 1 : 0) > f->cols) { emit_line(f); begin_line(f); }
    if (f->pending_space && f->lcols > f->indent) { put_b(f, ' '); f->lcols++; }
    f->pending_space = 0;
    put_b(f, (char)marker); put_b(f, (char)('0' + (idx >> 6))); put_b(f, (char)('0' + (idx & 63)));
    f->lcols += wcols;
    f->pending_space = 1;
}

/* весь текст внутри узла (для title, button) */
static void node_text(const struct node *n, char *out, size_t max, size_t *o) {
    for (n = n->first; n; n = n->next) {
        if (n->kind == 1 && n->text) {
            int sp = 0;
            for (const char *p = n->text; *p && *o + 1 < max; p++) {
                if (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') sp = 1;
                else { if (sp && *o) out[(*o)++] = ' '; sp = 0; if (*o + 1 < max) out[(*o)++] = *p; }
            }
            if (sp && *o && *o + 1 < max) out[(*o)++] = ' ';
        } else if (n->kind == 0 && strcmp(n->tag, "script") != 0 && strcmp(n->tag, "style") != 0) node_text(n, out, max, o);
    }
    out[*o] = 0;
}

/* действия при открытии тега; возвращает 0, если детей разбирать не нужно */
static int open_node(struct fb *f, const struct node *n, const struct cstyle *cs) {
    struct page *pg = f->pg;
    const char *name = n->tag;
    const char *tg = n->attrs ? n->attrs : "";
    size_t tn = n->attrs_len;
    char v[700];
    int inl = (cs->display == 2);                  /* display:inline — блочные теги не ломают строку */

    if (tag_in(name, "script style svg template iframe object select textarea noembed canvas head ")) {
        if (strcmp(name, "head") == 0) return 1;                   /* внутри head нас интересуют title/meta */
        return 0;
    }
    if (strcmp(name, "title") == 0) {
        char tmp[200]; size_t o = 0; tmp[0] = 0;
        node_text(n, tmp, sizeof tmp, &o);
        while (o && tmp[o - 1] == ' ') tmp[--o] = 0;
        size_t w = 0, tl = strlen(tmp);
        for (size_t k = 0; k < tl && w + 5 < sizeof pg->title; ) {
            uint32_t cp; size_t e = decode_entity(tmp, tl, k, &cp);
            if (e) k += e; else { const char *p = tmp + k; cp = utf8_next(&p); k = (size_t)(p - tmp); }
            cp = cp_fallback(cp);
            if (cp) w += (size_t)utf8_encode(cp, pg->title + w);
        }
        pg->title[w] = 0;
        return 0;
    }
    if (strcmp(name, "meta") == 0) {
        char eq[24];
        if (get_attr(tg, tn, "http-equiv", eq, sizeof eq) && ci_find(eq, strlen(eq), 0, "refresh") < strlen(eq) &&
            get_attr(tg, tn, "content", v, sizeof v)) {
            size_t u = ci_find(v, strlen(v), 0, "url=");
            if (u < strlen(v)) {
                const char *r = v + u + 4; if (*r == '\'' || *r == '"') r++;
                char tmp[640]; copy_str(tmp, sizeof tmp, r);
                size_t l = strlen(tmp); while (l && (tmp[l - 1] == '\'' || tmp[l - 1] == '"')) tmp[--l] = 0;
                if (net_url_join(f->base, tmp, pg->refresh, sizeof pg->refresh) != 0) pg->refresh[0] = 0;
            }
        }
        return 0;
    }
    if (strcmp(name, "br") == 0) { flush_word(f); hard_newline(f); f->pending_space = 0; return 0; }
    if (strcmp(name, "hr") == 0) {
        flush_word(f); br(f, 1);
        begin_line(f);
        uint32_t save = f->rgb; int sh = f->head; f->rgb = 0; f->head = 0; update_style(f);
        put_b(f, PGM_COLOR); put_b(f, '0' + PG_C_DIM); f->color_cur = PG_C_DIM;
        for (int i = f->indent; i < f->cols; i++) { put_b(f, '-'); f->lcols++; }
        f->rgb = save; f->head = sh; br(f, 1); return 0;
    }
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) { flush_word(f); br(f, 2); f->head++; update_style(f); return 1; }
    if (strcmp(name, "p") == 0) { flush_word(f); if (!inl) br(f, 2); return 1; }
    if (strcmp(name, "li") == 0) { flush_word(f); br(f, 1); append_raw(f, "• "); f->pending_space = 0; return 1; }
    if (tag_in(name, "ul ol dl menu ")) { flush_word(f); br(f, 1); f->indent += 2; return 1; }
    if (strcmp(name, "blockquote") == 0) { flush_word(f); br(f, 2); f->indent += 3; return 1; }
    if (strcmp(name, "pre") == 0) { flush_word(f); br(f, 2); f->pre = 1; f->code++; update_style(f); return 1; }
    if (tag_in(name, "code kbd tt samp ")) { flush_word(f); f->code++; update_style(f); return 1; }
    if (tag_in(name, "b strong th ")) { flush_word(f); f->bold++; update_style(f); return 1; }
    if (strcmp(name, "a") == 0) {
        if (get_attr(tg, tn, "href", v, sizeof v)) {
            int li = add_link(f, v);
            if (li >= 0) { flush_word(f); f->link = li; if (f->started) put_link_marker(f); update_style(f); }
        }
        return 1;
    }
    if (strcmp(name, "img") == 0) {
        char alt[100] = "";
        get_attr(tg, tn, "alt", alt, sizeof alt);
        if (!alt[0] && f->link >= 0) copy_str(alt, sizeof alt, "картинка");
        if (alt[0]) {
            flush_word(f);
            char lab[130]; ksnprintf(lab, sizeof lab, "[%s]", alt);
            char tmp[130]; size_t w = 0;
            for (const char *p = lab; *p && w + 5 < sizeof tmp; ) { uint32_t cp = cp_fallback(utf8_next(&p)); if (cp) w += (size_t)utf8_encode(cp, tmp + w); }
            tmp[w] = 0;
            put_word(f, tmp, utf8_len(tmp));
            f->pending_space = 1;
        }
        return 0;
    }
    if (strcmp(name, "form") == 0) {
        flush_word(f); br(f, 1);
        if (pg->nforms < PG_MAXFORMS) {
            struct pg_form *fm = &pg->forms[pg->nforms];
            memset(fm, 0, sizeof *fm);
            char act[640] = "", meth[12] = "";
            get_attr(tg, tn, "action", act, sizeof act);
            get_attr(tg, tn, "method", meth, sizeof meth);
            if (net_url_join(f->base, act, fm->action, sizeof fm->action) != 0) copy_str(fm->action, sizeof fm->action, f->base);
            fm->post = (lc(meth[0]) == 'p');
            f->form = pg->nforms++;
        }
        return 1;
    }
    if (strcmp(name, "input") == 0) {
        char type[16] = "", nm[40] = "", val[100] = "";
        get_attr(tg, tn, "type", type, sizeof type);
        get_attr(tg, tn, "name", nm, sizeof nm);
        get_attr(tg, tn, "value", val, sizeof val);
        for (char *c = type; *c; c++) *c = lc(*c);
        if (!strcmp(type, "hidden")) { add_input(f, IN_HIDDEN, nm, val, 0); return 0; }
        if (!strcmp(type, "submit") || !strcmp(type, "button") || !strcmp(type, "image")) {
            if (!strcmp(type, "button")) return 0;
            flush_word(f);
            int idx = add_input(f, IN_SUBMIT, nm, val[0] ? val : "OK", 0);
            if (idx >= 0) emit_widget(f, PGM_BUTTON, idx, (int)utf8_len(pg->inputs[idx].value) + 4);
            return 0;
        }
        if (!strcmp(type, "checkbox") || !strcmp(type, "radio") || !strcmp(type, "file") || !strcmp(type, "range") ||
            !strcmp(type, "color") || !strcmp(type, "reset")) return 0;
        flush_word(f);
        int idx = add_input(f, IN_TEXT, nm, val, !strcmp(type, "password"));
        if (idx >= 0) emit_widget(f, PGM_INPUT, idx, PG_FIELD_COLS);
        return 0;
    }
    if (strcmp(name, "button") == 0) {
        char lab[40], ty[12] = "submit", nm[40] = "", val[100] = "";
        size_t o = 0; lab[0] = 0;
        node_text(n, lab, sizeof lab, &o);
        while (o && lab[o - 1] == ' ') lab[--o] = 0;
        get_attr(tg, tn, "type", ty, sizeof ty);
        get_attr(tg, tn, "name", nm, sizeof nm);
        get_attr(tg, tn, "value", val, sizeof val);
        if (lc(ty[0]) == 'b' || lc(ty[0]) == 'r') return 0;
        flush_word(f);
        int idx = add_input(f, IN_SUBMIT, nm, lab[0] ? lab : (val[0] ? val : "OK"), 0);
        if (idx >= 0) emit_widget(f, PGM_BUTTON, idx, (int)utf8_len(pg->inputs[idx].value) + 4);
        return 0;
    }
    if (strcmp(name, "td") == 0) { flush_word(f); f->pending_space = 1; if (f->started) { put_b(f, ' '); f->lcols++; } return 1; }
    if (tag_in(name, "tr table div section article header footer nav main aside address figure figcaption fieldset legend dd dt summary details center caption body html ")) {
        flush_word(f);
        if (!inl) br(f, tag_in(name, "table ") ? 2 : 1);
        return 1;
    }
    if (cs->display == 1) { flush_word(f); br(f, 1); }             /* CSS: display:block у обычного тега */
    return 1;
}

static void close_node(struct fb *f, const struct node *n, const struct cstyle *cs) {
    const char *name = n->tag;
    int inl = (cs->display == 2);
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) { flush_word(f); if (f->head) f->head--; update_style(f); br(f, 2); return; }
    if (strcmp(name, "p") == 0) { flush_word(f); if (!inl) br(f, 2); return; }
    if (strcmp(name, "a") == 0) {
        flush_word(f);
        if (f->link >= 0) { f->link = -1; if (f->started) put_b(f, PGM_LINKEND); update_style(f); }
        return;
    }
    if (tag_in(name, "ul ol dl menu ")) { flush_word(f); br(f, 1); if (f->indent >= 2) f->indent -= 2; return; }
    if (strcmp(name, "blockquote") == 0) { flush_word(f); br(f, 2); if (f->indent >= 3) f->indent -= 3; return; }
    if (strcmp(name, "pre") == 0) { flush_word(f); br(f, 2); f->pre = 0; if (f->code) f->code--; update_style(f); return; }
    if (tag_in(name, "code kbd tt samp ")) { flush_word(f); if (f->code) f->code--; update_style(f); return; }
    if (tag_in(name, "b strong th ")) { flush_word(f); if (f->bold) f->bold--; update_style(f); return; }
    if (strcmp(name, "form") == 0) { flush_word(f); f->form = -1; br(f, 1); return; }
    if (tag_in(name, "tr table div section article header footer nav main aside address figure figcaption fieldset li dd dt details center caption ")) {
        flush_word(f);
        if (!inl) br(f, tag_in(name, "table ") ? 2 : 1);
        return;
    }
    if (cs->display == 1) { flush_word(f); br(f, 1); }
}

struct inh { uint32_t color; int8_t align; int8_t pre; };

static void walk(struct fb *f, const struct node *n, const struct inh *in, int depth) {
    for (; n; n = n->next) {
        if (n->kind == 1) { if (n->text) feed_text(f, n->text); continue; }
        if (n->hidden) continue;
        if (depth > MAX_DEPTH + 4) continue;
        struct cstyle cs;
        compute_style(f->css, n, &cs);
        if (cs.display == 0) continue;
        /* сохраняем состояние */
        uint32_t s_rgb = f->rgb; int s_align = f->align, s_nobold = f->nobold, s_pre = f->pre;
        struct inh child = *in;
        int is_a = strcmp(n->tag, "a") == 0;
        if (cs.color) child.color = cs.color;
        if (cs.align >= 0) child.align = cs.align;
        if (cs.pre >= 0) child.pre = cs.pre;
        flush_word(f);
        f->rgb = (is_a && !cs.color) ? 0 : child.color;
        int bold_added = 0;
        if (cs.bold == 1 && !tag_in(n->tag, "b strong th ")) { f->bold++; bold_added = 1; }
        else if (cs.bold == 0) f->nobold++;
        if (cs.pre == 1 && !f->pre) f->pre = 1;
        else if (cs.pre == 0 && f->pre && strcmp(n->tag, "pre") != 0) f->pre = 0;

        int descend = open_node(f, n, &cs);          /* переводы строк при открытии — ещё со старым выравниванием */
        f->align = child.align > 0 ? child.align : 0;
        update_style(f);
        if (descend && n->first) walk(f, n->first, &child, depth + 1);
        flush_word(f);
        close_node(f, n, &cs);
        if (bold_added && f->bold) f->bold--;
        f->nobold = s_nobold;
        f->rgb = s_rgb;
        f->pre = s_pre;
        f->align = s_align;
        update_style(f);
    }
}

static const struct node *find_tag(const struct node *n, const char *tag) {
    for (; n; n = n->next) {
        if (n->kind == 0 && strcmp(n->tag, tag) == 0) return n;
        const struct node *r = n->first ? find_tag(n->first, tag) : NULL;
        if (r) return r;
    }
    return NULL;
}

static int luma(uint32_t c) { return (int)(((c >> 16) & 255) * 30 + ((c >> 8) & 255) * 59 + (c & 255) * 11) / 100; }

void page_render(struct page *pg, const char *h, size_t n, int cols, const char *base) {
    /* ВАЖНО: обнуляем всю структуру целиком, прежде чем что-либо в неё писать.
       pg->nlines/nlinks/ninputs/nforms — счётчики, растущие через ...++ по ходу разбора;
       если вызывающий код объявил `struct page pg;` на стеке без инициализации (обычное дело),
       эти счётчики стартуют с мусора со стека, и запись pg->forms[pg->nforms] уходит куда попало —
       результат то работает, то нет, в зависимости от того, что раньше лежало на стеке. */
    memset(pg, 0, sizeof *pg);
    struct node *root = dom_build(h, n);
    if (!root) return;
    struct css css = { 0, 0 };
    css.r = kmalloc(sizeof(struct css_rule) * CSS_MAXRULES);
    if (css.r) css_collect(&css, root);
    struct fb *f = kmalloc(sizeof *f);
    if (!f) { free_tree(root); if (css.r) css_free(&css); return; }
    memset(f, 0, sizeof *f);
    f->pg = pg; f->cols = cols; f->link = -1; f->form = -1; f->base = base; f->css = &css;
    pg->bg = 0xFFFFFF;
    /* фон страницы: body, иначе html */
    const char *bt[2] = { "body", "html" };
    for (int i = 0; i < 2; i++) {
        const struct node *bn = find_tag(root, bt[i]);
        if (!bn) continue;
        struct cstyle cs; compute_style(&css, bn, &cs);
        if (cs.bg) { pg->bg = cs.bg & 0xFFFFFF; break; }
    }
    pg->dark = luma(pg->bg) < 110;
    struct inh in0 = { 0, 0, 0 };
    walk(f, root->first, &in0, 0);
    flush_word(f);
    if (f->lcols > 0 || f->lb > 0) emit_line(f);
    kfree(f);
    if (css.r) css_free(&css);
    free_tree(root);
}

void page_render_text(struct page *pg, const char *t, size_t n, int cols) {
    memset(pg, 0, sizeof *pg);   /* тот же случай, что и в page_render — см. комментарий там */
    struct fb *f = kmalloc(sizeof *f);
    if (!f) return;
    memset(f, 0, sizeof *f);
    f->pg = pg; f->cols = cols; f->link = -1; f->form = -1; f->pre = 1;
    pg->bg = 0xFFFFFF;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)t[i];
        if (c == '\n') emit_line(f);
        else if (c == '\r' || (c < 0x20 && c != '\t')) { }
        else {
            begin_line(f);
            int cnt = (c == '\t') ? 4 : 1;
            while (cnt--) {
                if (f->lcols >= cols) { emit_line(f); begin_line(f); }
                if (c == '\t') { put_b(f, ' '); f->lcols++; }
                else {
                    const char *s = t + i, *s0 = s;
                    uint32_t cp = utf8_next(&s);
                    if (i + (size_t)(s - s0) > n) s = s0 + 1;
                    cp = cp_fallback(cp);
                    if (cp >= 0x20) { char tmp[5]; int l = utf8_encode(cp, tmp); for (int k = 0; k < l; k++) put_b(f, tmp[k]); f->lcols++; }
                    i += (size_t)(s - s0) - 1;
                }
            }
        }
    }
    if (f->lb > 0) emit_line(f);
    kfree(f);
}

void page_free(struct page *pg) {
    for (int i = 0; i < pg->nlines; i++) kfree(pg->lines[i]);
    for (int i = 0; i < pg->nlinks; i++) kfree(pg->links[i]);
    memset(pg, 0, sizeof *pg);
}

/* ================= формы ================= */

void url_encode(const char *s, char *out, size_t max) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *s && o + 4 < max; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else if (c == ' ') out[o++] = '+';
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

int page_form_url(const struct page *pg, int form, int clicked, char *out, size_t max) {
    if (form < 0 || form >= pg->nforms) return -1;
    size_t o = 0;
    const char *a = pg->forms[form].action;
    while (*a && *a != '#' && o + 1 < max) out[o++] = *a++;
    int has_q = 0; for (size_t k = 0; k < o; k++) if (out[k] == '?') has_q = 1;
    int first = 1;
    for (int i = 0; i < pg->ninputs; i++) {
        const struct pg_input *in = &pg->inputs[i];
        if (in->form != form || !in->name[0]) continue;
        if (in->kind == IN_SUBMIT && i != clicked) continue;
        char nm[100], vl[300];
        url_encode(in->name, nm, sizeof nm);
        url_encode(in->value, vl, sizeof vl);
        char sep = first ? (has_q ? '&' : '?') : '&';
        first = 0;
        if (o + 2 + strlen(nm) + strlen(vl) + 1 >= max) break;
        out[o++] = sep;
        for (const char *p = nm; *p; ) out[o++] = *p++;
        out[o++] = '=';
        for (const char *p = vl; *p; ) out[o++] = *p++;
    }
    out[o] = 0;
    return 0;
}

/* ================= кодировка ================= */

char *cp1251_to_utf8(const char *s, size_t n, size_t *out_len) {
    char *o = kmalloc(n * 3 + 1);
    if (!o) return NULL;
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        if (c < 0x80) cp = c;
        else if (c >= 0xC0) cp = 0x410 + (c - 0xC0);
        else if (c == 0xA8) cp = 0x401;
        else if (c == 0xB8) cp = 0x451;
        else if (c == 0x96) cp = 0x2013;
        else if (c == 0x97) cp = 0x2014;
        else if (c == 0xAB) cp = 0xAB;
        else if (c == 0xBB) cp = 0xBB;
        else if (c == 0xA0) cp = ' ';
        else if (c == 0xB9) cp = 0x2116;
        else cp = '?';
        w += (size_t)utf8_encode(cp, o + w);
    }
    o[w] = 0;
    *out_len = w;
    return o;
}
