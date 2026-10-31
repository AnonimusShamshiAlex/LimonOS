/* Выбор сетевой карты: сначала Intel e1000 (VirtualBox, QEMU, многие платы), затем Realtek RTL8139
   (очень частая на старых ПК и в QEMU: -device rtl8139). */
#include "nic.h"
#include "nic_drv.h"

static int active;   /* 0 нет, 1 e1000, 2 rtl8139 */

int nic_init(void) {
    if (e1000_init() == 0)  { active = 1; return 0; }
    if (rtl8139_init() == 0) { active = 2; return 0; }
    active = 0;
    return -1;
}
const char *nic_name(void) { return active == 1 ? e1000_name() : active == 2 ? rtl8139_name() : "нет"; }
const uint8_t *nic_mac(void) { static const uint8_t z[6]; return active == 1 ? e1000_mac() : active == 2 ? rtl8139_mac() : z; }
int nic_link(void) { return active == 1 ? e1000_link() : active == 2 ? rtl8139_link() : 0; }
int nic_send(const void *f, size_t n) { return active == 1 ? e1000_send(f, n) : active == 2 ? rtl8139_send(f, n) : -1; }
int nic_recv(void *b, size_t max) { return active == 1 ? e1000_recv(b, max) : active == 2 ? rtl8139_recv(b, max) : 0; }
