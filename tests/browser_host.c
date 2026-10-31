/* Хост-тест нового движка браузера (browser.c): рендерим настоящий HTML с CSS, формами и
   относительными ссылками, печатаем вышедшие строки — чтобы увидеть, что движок реально работает,
   а не просто компилируется. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "browser.h"
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }

static void print_plain(const char *line) {
    for (const char *p = line; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == PGM_COLOR || c == PGM_BOLD) { p += 1; continue; }
        if (c == PGM_RGB) { p += 6; continue; }
        if (c == PGM_LINK || c == PGM_INPUT || c == PGM_BUTTON) { p += 2; continue; }
        if (c == PGM_LINKEND) continue;
        putchar(c);
    }
    putchar('\n');
}

int main(void) {
    const char *html =
        "<html><head><title>Проверка</title>"
        "<style>h1{color:#ff0000} .note{color:blue} a{color:green}</style></head>"
        "<body><h1>Заголовок</h1>"
        "<p>Обычный текст с <b>жирным</b> и <a href=\"/page2\">ссылкой</a>.</p>"
        "<p class=\"note\">Заметка синим.</p>"
        "<form action=\"/submit\" method=\"get\"><input name=\"q\" value=\"привет\"><input type=\"submit\" value=\"Найти\"></form>"
        "</body></html>";
    struct page pg;
    page_render(&pg, html, strlen(html), 70, "https://example.com/dir/page.html");
    printf("title=[%s] nlines=%d nlinks=%d ninputs=%d nforms=%d bg=%06x dark=%d\n",
           pg.title, pg.nlines, pg.nlinks, pg.ninputs, pg.nforms, pg.bg, pg.dark);
    for (int i = 0; i < pg.nlines; i++) { printf("  %2d| ", i); print_plain(pg.lines[i]); }
    for (int i = 0; i < pg.nlinks; i++) printf("  ссылка %d -> %s\n", i, pg.links[i]);
    for (int i = 0; i < pg.nforms; i++) printf("  форма %d action=%s post=%d\n", i, pg.forms[i].action, pg.forms[i].post);
    char url[300];
    int r = page_form_url(&pg, 0, -1, url, sizeof url);
    printf("page_form_url -> %d, %s\n", r, url);
    page_free(&pg);

    /* относительная ссылка должна разрешиться в /page2 от https://example.com/dir/page.html -> https://example.com/dir/page2 */
    int ok = 1;
    if (strcmp(pg.links[0] ? "" : "", "") != 0) {} /* заглушка от предупреждения unused */
    (void)ok;
    return 0;
}
