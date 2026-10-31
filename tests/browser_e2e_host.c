/* Сквозной тест: desktop.c НЕ собрать на хосте (нужна графика ядра), поэтому проверяем ровно то,
   что реально меняли: http_fetch (мой, проверенный) -> page_render (перенесённый движок) на
   настоящей HTML-странице, отданной тестовым сервером через TAP. */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>

static void crash_handler(int sig) {
    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "\n=== СИГНАЛ %d, трассировка (%d кадров) ===\n", sig, n);
    backtrace_symbols_fd(bt, n, 2);
    _exit(139);
}
#include "net.h"
#include "http.h"
#include "tls.h"
#include "browser.h"
void *kmalloc(size_t n); void kfree(void *p);
#include <stdlib.h>
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }

static void print_plain(const char *line) {
    for (const unsigned char *p = (const unsigned char *)line; *p; p++) {
        unsigned char c = *p;
        if (c == PGM_COLOR || c == PGM_BOLD) { p += 1; continue; }
        if (c == PGM_RGB) { p += 6; continue; }
        if (c == PGM_LINK || c == PGM_INPUT || c == PGM_BUTTON) { p += 2; continue; }
        if (c == PGM_LINKEND) continue;
        putchar(c);
    }
    putchar('\n');
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGSEGV, crash_handler);
    net_init();
    uint32_t ip, mask, gw; net_parse_ip("10.0.2.15", &ip); net_parse_ip("255.255.255.0", &mask); net_parse_ip("10.0.2.1", &gw);
    net_set_static(ip, mask, gw, gw);
    struct http_opts o; memset(&o, 0, sizeof o);
    o.max_bytes = 2u << 20;
    struct http_resp r;
    int rc = http_fetch(argv[1], &o, &r);
    if (rc != HTTP_OK) { printf("http_fetch ОШИБКА %d: %s\n", rc, http_strerror(rc)); return 1; }
    printf("статус=%d тип=%s итоговый_url=%s длина=%zu\n", r.status, r.content_type, r.url, r.len);
    struct page pg;
    page_render(&pg, (const char *)r.data, r.len, 70, r.url);
    printf("--- title=[%s] nlines=%d nlinks=%d bg=%06x dark=%d ---\n", pg.title, pg.nlines, pg.nlinks, pg.bg, pg.dark);
    for (int i = 0; i < pg.nlines && i < 20; i++) { printf("%3d| ", i); print_plain(pg.lines[i]); }
    for (int i = 0; i < pg.nlinks; i++) printf("ссылка %d -> %s\n", i, pg.links[i]);
    printf("метка A: перед page_free\n"); fflush(stdout);
    page_free(&pg);
    printf("метка B: после page_free, перед http_free\n"); fflush(stdout);
    http_free(&r);
    printf("метка C: после http_free\n"); fflush(stdout);
    return 0;
}
