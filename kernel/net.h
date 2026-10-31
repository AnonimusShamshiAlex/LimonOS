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

/* ---- TCP: надёжный клиент (повторы, окно, 64 КБ приёмный буфер) ---- */
int  tcp_connect(uint32_t ip, uint16_t port, int timeout_ms);   /* id соединения, или -1 нет слотов, -2 не отвечает, -3 порт закрыт (RST), -4 нет памяти */
int  tcp_send(int id, const void *data, size_t len);            /* 0 — принято к отправке (ждёт, если буфер полон) */
int  tcp_recv(int id, void *buf, size_t max, int timeout_ms);   /* >0 байт, 0 таймаут, -1 собеседник закрыл, -2 обрыв */
void tcp_close(int id);
int  tcp_info(int id, uint32_t *ip, uint16_t *port, int *state, uint32_t *queued);
void net_set_static(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns);

/* склеивает базовый адрес страницы и ссылку/действие формы (как это делает браузер); фрагмент #... отбрасывается; 0 — успех */
int  net_url_join(const char *base, const char *ref, char *out, size_t max);

/* достаёт значение заголовка (например "Location") из ответа http_get; 0 — нашёл */
int  http_header(const char *response, size_t headers_len, const char *name, char *out, size_t outmax);

#endif
