/* Хост-тест ai.c: отправляет вопрос на локальный имитационный сервер (вместо api.anthropic.com),
   проверяет, что JSON собрался правильно и что разбор ответа (включая экранирование и \u) работает. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net.h"
#include "ai.h"
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
static int nlines = 0;
static void on_line(const char *s, void *ctx) { (void)ctx; printf("LINE: %s\n", s); nlines++; }
int main(int argc, char **argv) {
    if (net_init() != 0) { fprintf(stderr, "нет TAP\n"); return 2; }
    uint32_t ip, mask, gw; net_parse_ip("10.0.2.15", &ip); net_parse_ip("255.255.255.0", &mask); net_parse_ip("10.0.2.1", &gw);
    net_set_static(ip, mask, gw, gw);
    ai_set_key(argv[1]);
    int rc = ai_ask(argv[2], on_line, NULL);
    printf("rc=%d lines=%d\n", rc, nlines);
    return 0;
}
