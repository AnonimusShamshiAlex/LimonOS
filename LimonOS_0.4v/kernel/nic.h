#ifndef LIMON_NIC_H
#define LIMON_NIC_H
#include <stddef.h>
#include <stdint.h>

/* Интерфейс сетевой карты. Сейчас есть драйвер e1000 (его использует VirtualBox). */
int         nic_init(void);              /* 0 — карта найдена и запущена */
const char *nic_name(void);
const uint8_t *nic_mac(void);
int         nic_link(void);              /* 1 — кабель/канал подключён */
int         nic_send(const void *frame, size_t len);   /* 0 — отправлено */
int         nic_recv(void *buf, size_t max);           /* длина кадра или 0 */

#endif
