#ifndef LIMON_NET_H
#define LIMON_NET_H
#include <stdint.h>
#include <stddef.h>

/* Vine — сетевой стек LimonOS: Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS. IP-адреса — числа вида a<<24|b<<16|c<<8|d */
struct netinfo {
    int present, link, configured;
    uint8_t mac[6];
    uint32_t ip, mask, gw, dns;
    const char *nic;
};

int  net_init(void);                              /* 0 — сетевая карта запущена */
const struct netinfo *net_info(void);
void net_poll(void);                              /* принять и обработать пришедшие пакеты */
int  net_dhcp(void);                              /* получить адрес автоматически, 0 — успех */
int  net_ping(uint32_t ip, int timeout_ms, int *rtt_ms);   /* 0 — ответ получен */
int  net_resolve(const char *host, uint32_t *ip); /* имя -> адрес; 0 — успех */
int  net_parse_ip(const char *s, uint32_t *ip);   /* "10.0.2.15" -> число; 0 — успех */
void net_ip_str(uint32_t ip, char out[16]);

/* ---- TCP (простой клиент: только исходящие соединения, без повторной отправки и переупорядочивания) ---- */
int  tcp_connect(uint32_t ip, uint16_t port, int timeout_ms);   /* id соединения или <0 */
int  tcp_send(int id, const void *data, size_t len);
int  tcp_read_all(int id, void *buf, size_t maxlen, int timeout_ms);  /* читает до FIN или таймаута */
void tcp_close(int id);

/* ---- HTTP (поверх TCP, только http://, без перенаправлений и TLS) ---- */
int  http_get(const char *host, uint16_t port, const char *path,
              char *out, size_t outmax, int *status, size_t *body_off);
/* достаёт значение заголовка (например "Location") из ответа http_get; 0 — нашёл */
int  http_header(const char *response, size_t headers_len, const char *name, char *out, size_t outmax);

#endif
