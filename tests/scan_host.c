/* Хост-тест сканера портов: tcp_connect к нескольким портам, сверяем классификацию
   (открыт / закрыт-RST / не отвечает) с тем, что реально слушает на хосте. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net.h"
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }

int main(int argc, char **argv) {
    if (net_init() != 0) { fprintf(stderr, "нет TAP\n"); return 2; }
    uint32_t ip, mask, gw;
    net_parse_ip("10.0.2.15", &ip); net_parse_ip("255.255.255.0", &mask); net_parse_ip("10.0.2.1", &gw);
    net_set_static(ip, mask, gw, gw);
    uint32_t dst; net_parse_ip(argv[1], &dst);
    for (int i = 2; i < argc; i++) {
        int port = atoi(argv[i]);
        int id = tcp_connect(dst, (uint16_t)port, 1200);
        const char *s = id >= 0 ? "открыт" : (id == -3 ? "закрыт(RST)" : (id == -2 ? "не отвечает" : "ошибка"));
        printf("%d -> %s\n", port, s);
        if (id >= 0) tcp_close(id);
    }
    return 0;
}
