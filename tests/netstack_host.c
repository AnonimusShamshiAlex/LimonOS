/* Настоящий стек LimonOS (net.c: ARP/IP/TCP, tls.c, http.c) поверх TAP на хосте.
   Использование: netstack_host <url> <файл-для-тела> [POST файл-с-телом] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net.h"
#include "http.h"
#include "tls.h"
#include "crypto.h"

void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
extern long dropped_tx, dropped_rx, sent_frames;

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    if (net_init() != 0) { fprintf(stderr, "нет TAP (запусти run_netstack_test.sh)\n"); return 2; }
    uint32_t ip, mask, gw;
    net_parse_ip("10.0.2.15", &ip); net_parse_ip("255.255.255.0", &mask); net_parse_ip("10.0.2.1", &gw);
    net_set_static(ip, mask, gw, gw);
    rng_add_entropy("netstack-host", 13);
    struct http_opts o; memset(&o, 0, sizeof o);
    o.max_bytes = 32u << 20;
    if (getenv("TMO")) o.timeout_ms = atoi(getenv("TMO"));
    char *body = NULL;
    if (argc >= 5 && strcmp(argv[3], "POST") == 0) {
        FILE *f = fopen(argv[4], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        body = malloc((size_t)n); if (fread(body, 1, (size_t)n, f) != (size_t)n) return 9; fclose(f);
        o.method = "POST"; o.body = body; o.body_len = (size_t)n; o.content_type = "application/octet-stream";
    }
    struct http_resp r;
    int rc = http_fetch(argv[1], &o, &r);
    if (rc != HTTP_OK) {
        fprintf(stderr, "ОШИБКА %d: %s%s%s\n", rc, http_strerror(rc), rc == HTTP_E_TLS ? " / " : "", rc == HTTP_E_TLS ? tls_strerror(r.tls_err) : "");
        return 3;
    }
    FILE *f = fopen(argv[2], "wb"); fwrite(r.data, 1, r.len, f); fclose(f);
    fprintf(stderr, "status=%d len=%zu https=%d cipher=%s redirects=%d url=%s | кадров %ld, потеряно tx=%ld rx=%ld\n",
            r.status, r.len, r.https, r.cipher, r.redirects, r.url, sent_frames, dropped_tx, dropped_rx);
    return 0;
}
