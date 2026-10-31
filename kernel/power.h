#ifndef LIMON_POWER_H
#define LIMON_POWER_H
void power_reboot(void);     /* не возвращается */
void power_shutdown(void);   /* если вернулась — выключить не удалось */
#endif
