#ifndef LIMON_NET_H
#define LIMON_NET_H
#include <stdint.h>

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

#endif
