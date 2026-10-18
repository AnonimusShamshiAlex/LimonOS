#include "browser.h"
#include "mem.h"
#include "kstring.h"

struct ent { const char *name; uint32_t cp; };
static const struct ent entities[] = {
    { "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' }, { "quot;", '"' },
    { "apos;", '\'' }, { "#39;", '\'' }, { "nbsp;", ' ' }, { "mdash;", 0x2014 },
    { "ndash;", 0x2013 }, { "hellip;", 0x2026 }, { "laquo;", 0xAB }, { "raquo;", 0xBB },
};

static const char *const block_tags[] = {
    "p", "br", "div", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6", "table", "ul", "ol",
};

static int tag_is_block(const char *name, int len) {
    for (unsigned i = 0; i < sizeof block_tags / sizeof block_tags[0]; i++)
        if ((int)strlen(block_tags[i]) == len && strncmp(block_tags[i], name, (size_t)len) == 0) return 1;
    return 0;
}

/* индекс needle в h (без учёта регистра), ищет начиная с from; n, если не найдено */
static size_t ci_find(const char *h, size_t n, size_t from, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0 || from + nl > n) return n;
    for (size_t i = from; i + nl <= n; i++) {
        size_t j = 0;
        for (; j < nl; j++) {
            char a = h[i + j]; if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            char b = needle[j];
            if (a != b) break;
        }
        if (j == nl) return i;
    }
    return n;
}

static int starts_tag_ci(const char *h, size_t n, size_t i, const char *name) {
    size_t l = strlen(name);
    if (i + 1 + l > n) return 0;
    for (size_t j = 0; j < l; j++) {
        char a = h[i + 1 + j]; if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != name[j]) return 0;
    }
    char after = (i + 1 + l < n) ? h[i + 1 + l] : '>';
    return after == '>' || after == ' ' || after == '\t' || after == '\n' || after == '/';
}

/* html -> обычный текст (без тегов), с переводами строк на блочных тегах.
 * Содержимое <script> и <style> целиком вырезается поиском закрывающего тега,
 * чтобы символ '<' внутри самого JS/CSS (например "x<2") не ломал разбор. */
static char *strip_html(const char *h, size_t n, size_t *out_len) {
    size_t cap = n + 64;
    char *out = kmalloc(cap);
    if (!out) { *out_len = 0; return NULL; }
    size_t o = 0;
    int in_tag = 0;
    int last_space = 1;
    char tagname[16]; int tnlen = 0; int tag_closing = 0;
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)h[i];
        if (!in_tag && c == '<') {
            if (starts_tag_ci(h, n, i, "script") || starts_tag_ci(h, n, i, "style")) {
                int is_script = starts_tag_ci(h, n, i, "script");
                size_t gt = i;
                while (gt < n && h[gt] != '>') gt++;
                size_t close = ci_find(h, n, gt, is_script ? "</script>" : "</style>");
                i = (close < n) ? close + (is_script ? 9 : 8) : n;
                if (!last_space && o < cap - 1) { out[o++] = ' '; last_space = 1; }
                continue;
            }
            in_tag = 1; tnlen = 0; tag_closing = 0;
            i++;
            if (i < n && h[i] == '/') { tag_closing = 1; i++; }
            continue;
        }
        if (in_tag) {
            if (c == '>') {
                in_tag = 0;
                tagname[tnlen < 15 ? tnlen : 15] = 0;
                (void)tag_closing;
                if (tag_is_block(tagname, tnlen)) {
                    if (o > 0 && out[o - 1] != '\n') out[o++] = '\n';
                    last_space = 1;
                } else if (!last_space && o < cap - 1) {
                    out[o++] = ' '; last_space = 1;
                }
                i++;
                continue;
            }
            if (tnlen < 15 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
                tagname[tnlen++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            }
            i++;
            continue;
        }
        if (c == '&') {
            unsigned j;
            int matched = 0;
            for (j = 0; j < sizeof entities / sizeof entities[0]; j++) {
                size_t l = strlen(entities[j].name);
                if (i + 1 + l <= n && strncmp(h + i + 1, entities[j].name, l) == 0) {
                    if (o + 4 < cap) o += (size_t)utf8_encode(entities[j].cp, out + o);
                    i += 1 + l;
                    matched = 1;
                    last_space = (entities[j].cp == ' ');
                    break;
                }
            }
            if (matched) continue;
            if (i + 1 < n && h[i + 1] == '#') {
                size_t k = i + 2;
                int hex = (k < n && (h[k] == 'x' || h[k] == 'X'));
                if (hex) k++;
                uint32_t cp = 0; size_t start = k;
                while (k < n && h[k] != ';' &&
                       ((h[k] >= '0' && h[k] <= '9') || (hex && ((h[k] | 32) >= 'a' && (h[k] | 32) <= 'f')))) {
                    int d = (h[k] <= '9') ? h[k] - '0' : (h[k] | 32) - 'a' + 10;
                    cp = cp * (uint32_t)(hex ? 16 : 10) + (uint32_t)d;
                    k++;
                }
                if (k > start && k < n && h[k] == ';' && o + 4 < cap) {
                    o += (size_t)utf8_encode(cp, out + o);
                    i = k + 1;
                    last_space = 0;
                    continue;
                }
            }
            if (o < cap - 1) { out[o++] = '&'; last_space = 0; }
            i++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!last_space && o < cap - 1) { out[o++] = ' '; last_space = 1; }
            i++;
            continue;
        }
        if (o < cap - 1) { out[o++] = (char)c; last_space = 0; }
        i++;
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\n')) o--;
    out[o] = 0;
    *out_len = o;
    return out;
}

struct wrapper {
    char **out; int n, max;
    char buf[600]; int cols; size_t bytes;
};

static void wrap_flush(struct wrapper *w) {
    if (w->n >= w->max) return;
    w->buf[w->bytes] = 0;
    char *s = kmalloc(w->bytes + 1);
    if (s) { memcpy(s, w->buf, w->bytes + 1); w->out[w->n++] = s; }
    w->cols = 0; w->bytes = 0;
}

int html_to_lines(const char *html, size_t html_len, int cols, char **out_lines, int max_lines) {
    size_t tlen;
    char *text = strip_html(html, html_len, &tlen);
    if (!text) return 0;

    struct wrapper w = { out_lines, 0, max_lines, { 0 }, 0, 0 };
    const char *p = text;
    int blank_run = 0;

    while (*p && w.n < max_lines) {
        if (*p == '\n') {
            wrap_flush(&w);
            p++;
            blank_run++;
            if (blank_run > 1) continue;          /* не больше одной пустой строки подряд */
            if (w.n < max_lines) { char *e = kmalloc(1); if (e) { e[0] = 0; out_lines[w.n++] = e; } }
            continue;
        }
        blank_run = 0;
        if (*p == ' ') { p++; continue; }           /* ведущие пробелы новой строки пропускаем */
        const char *word = p;
        int wcols = 0;
        while (*word && *word != ' ' && *word != '\n') { utf8_next(&word); wcols++; }
        size_t wbytes = (size_t)(word - p);

        if (w.cols > 0 && w.cols + 1 + wcols > cols) wrap_flush(&w);
        else if (w.cols > 0) { w.buf[w.bytes++] = ' '; w.cols++; }

        while (wcols > cols) {                      /* слово длиннее строки — режем жёстко */
            int take = cols - w.cols;
            if (take <= 0) { wrap_flush(&w); take = cols; }
            while (take-- > 0 && wbytes > 0) {
                const char *q = p; utf8_next(&q);
                size_t cb = (size_t)(q - p);
                if (w.bytes + cb >= sizeof w.buf - 1) break;
                memcpy(w.buf + w.bytes, p, cb); w.bytes += cb; w.cols++;
                p += cb; wbytes -= cb; wcols--;
            }
            wrap_flush(&w);
        }
        if (w.bytes + wbytes < sizeof w.buf - 1) { memcpy(w.buf + w.bytes, p, wbytes); w.bytes += wbytes; w.cols += wcols; }
        p += wbytes;
    }
    if (w.cols > 0) wrap_flush(&w);
    kfree(text);
    return w.n;
}
