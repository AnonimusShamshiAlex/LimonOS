#include <stdio.h>
#include <string.h>
#include "net.h"
#include "http.h"
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
int main(int argc, char **argv) {
    net_init();
    uint32_t ip, mask, gw; net_parse_ip("10.0.2.15", &ip); net_parse_ip("255.255.255.0", &mask); net_parse_ip("10.0.2.1", &gw);
    net_set_static(ip, mask, gw, gw);
    struct http_opts o; memset(&o, 0, sizeof o);
    o.method = "POST";
    const char *body = "{\"model\":\"claude-haiku-5-5\",\"max_tokens\":1024,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}";
    o.body = body; o.body_len = strlen(body); o.content_type = "application/json";
    o.extra_headers = "x-api-key: sk-ant-TEST123\r\nanthropic-version: 2023-06-01\r\n";
    o.max_bytes = 1<<20;
    struct http_resp r;
    int rc = http_fetch(argv[1], &o, &r);
    printf("rc=%d status=%d len=%zu\n", rc, r.status, r.len);
    fwrite(r.data, 1, r.len, stdout); printf("\n");
    return 0;
}
