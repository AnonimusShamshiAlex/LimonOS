#ifndef LIMON_NIC_DRV_H
#define LIMON_NIC_DRV_H
#include <stddef.h>
#include <stdint.h>
/* Внутренний интерфейс драйверов сетевых карт; nic.c выбирает подходящий. */
int e1000_init(void);  const char *e1000_name(void);  const uint8_t *e1000_mac(void);  int e1000_link(void);
int e1000_send(const void *f, size_t n);  int e1000_recv(void *b, size_t max);
int rtl8139_init(void); const char *rtl8139_name(void); const uint8_t *rtl8139_mac(void); int rtl8139_link(void);
int rtl8139_send(const void *f, size_t n); int rtl8139_recv(void *b, size_t max);
#endif
