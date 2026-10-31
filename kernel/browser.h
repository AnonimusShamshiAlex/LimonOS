#ifndef LIMON_BROWSER_H
#define LIMON_BROWSER_H
#include <stddef.h>
#include <stdint.h>

/* Citrus 2 — движок страниц: разбирает HTML в готовые строки с оформлением, ссылками и формами.
 * Устройство: HTML -> дерево DOM -> CSS (селекторы тег/#id/.класс/потомок/дочерний, специфичность, !important,
 * display/visibility/opacity, color, background, font-weight, text-align, white-space) -> строки с разметкой.
 * Чего НЕТ: внешние файлы CSS (<link>), float/position/размеры, JavaScript, картинки (пока).
 *
 * В строке, кроме текста UTF-8, бывают служебные коды (в обычном тексте их нет):
 *   PGM_COLOR c      — сменить цвет ('0'+PG_C_*)
 *   PGM_LINK  h l    — начало ссылки №((h-'0')<<6 | (l-'0'))
 *   PGM_LINKEND      — конец ссылки
 *   PGM_INPUT h l    — поле ввода №..., ширина PG_FIELD_COLS столбцов
 *   PGM_BUTTON h l   — кнопка №...; ширина = длина подписи + 4 */
#define PGM_COLOR   1
#define PGM_LINK    2
#define PGM_LINKEND 3
#define PGM_INPUT   4
#define PGM_BUTTON  5
#define PGM_RGB     6     /* + 6 шестнадцатеричных цифр RRGGBB — произвольный цвет текста */
#define PGM_BOLD    7     /* + '1' жирный включить, '0' выключить */

enum { PG_C_TEXT = 0, PG_C_HEAD, PG_C_LINK, PG_C_BOLD, PG_C_DIM, PG_C_CODE };

#define PG_MAXLINES  4000
#define PG_MAXLINKS  600
#define PG_MAXINPUTS 64
#define PG_MAXFORMS  16
#define PG_FIELD_COLS 26

enum { IN_HIDDEN = 0, IN_TEXT, IN_SUBMIT };

struct pg_input {
    char name[32];
    char value[96];
    uint8_t kind, password;
    int form;
};
struct pg_form { char action[640]; int post; };

struct page {
    char *lines[PG_MAXLINES];
    int nlines;
    char *links[PG_MAXLINKS];            /* готовые абсолютные адреса */
    int nlinks;
    struct pg_input inputs[PG_MAXINPUTS];
    int ninputs;
    struct pg_form forms[PG_MAXFORMS];
    int nforms;
    char title[80];
    uint32_t bg;                         /* цвет фона страницы 0xRRGGBB (из CSS/bgcolor); 0xFFFFFF по умолчанию */
    int dark;                            /* фон тёмный — палитра по умолчанию светлая */
    char refresh[640];                   /* адрес из <meta http-equiv=refresh>, если есть */
};

/* html — текст в UTF-8; base — адрес страницы (для относительных ссылок); cols — ширина в символах */
void page_render(struct page *pg, const char *html, size_t len, int cols, const char *base);
void page_render_text(struct page *pg, const char *text, size_t len, int cols);   /* text/plain */
void page_free(struct page *pg);

/* собирает адрес отправки формы (GET); clicked — номер нажатой кнопки или -1 */
int  page_form_url(const struct page *pg, int form, int clicked, char *out, size_t max);
void url_encode(const char *s, char *out, size_t max);

/* windows-1251 -> UTF-8 (новая память, освободить kfree) */
char *cp1251_to_utf8(const char *s, size_t n, size_t *out_len);

#endif
