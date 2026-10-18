#ifndef LIMON_BROWSER_H
#define LIMON_BROWSER_H
#include <stddef.h>

/* Citrus — простой текстовый браузер: убирает теги HTML, раскрывает несколько
 * сущностей (&amp; &lt; ...), сворачивает пробелы и переносит строки по ширине.
 * cols — ширина строки в символах. Возвращает количество готовых строк.
 * out_lines — массив указателей на kmalloc'нутые строки (заполняет функция),
 * max_lines — размер этого массива. */
int html_to_lines(const char *html, size_t html_len, int cols, char **out_lines, int max_lines);

#endif
