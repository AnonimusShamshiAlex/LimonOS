/* Подмена сетевой карты и часов для хост-теста: кадры Ethernet идут в TAP-интерфейс Linux.
   Переменная окружения LOSS=N — терять N% пакетов в обе стороны (проверка повторов TCP). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include "nic.h"
#include "timer.h"

volatile uint64_t timer_ticks;
void timer_init(void) {}
void timer_irq(void) {}
static int fd = -1, loss;
static uint8_t mymac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static void trace(const char *dir, const uint8_t *f, size_t n, int lost) {
    if (!getenv("TRACE") || n < 54 || f[12] != 8 || f[13] != 0 || f[23] != 6) return;
    const uint8_t *t = f + 14 + (f[14] & 15) * 4;
    uint32_t seq = (uint32_t)t[4] << 24 | t[5] << 16 | t[6] << 8 | t[7], ack = (uint32_t)t[8] << 24 | t[9] << 16 | t[10] << 8 | t[11];
    size_t hl = (t[12] >> 4) * 4, tl = n - 14 - (f[14] & 15) * 4;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "%6ld.%03ld %s%s fl=%02x seq=%u ack=%u len=%zu\n", ts.tv_sec % 1000, ts.tv_nsec / 1000000, dir, lost ? " LOST" : "      ", t[13], seq, ack, tl - hl);
}
long dropped_tx, dropped_rx, sent_frames;

static void tick(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    timer_ticks = (uint64_t)ts.tv_sec * 100 + (uint64_t)ts.tv_nsec / 10000000;
}

int nic_init(void) {
    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) return -1;
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    strncpy(ifr.ifr_name, "limon0", IFNAMSIZ - 1);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) return -2;
    const char *l = getenv("LOSS"); loss = l ? atoi(l) : 0;
    srand(12345);
    tick();
    return 0;
}
const char *nic_name(void) { return "tap (хост-тест)"; }
const uint8_t *nic_mac(void) { return mymac; }
int nic_link(void) { return 1; }
int nic_send(const void *frame, size_t len) {
    sent_frames++;
    if (loss && rand() % 100 < loss) { dropped_tx++; trace("TX", frame, len, 1); return 0; }
    trace("TX", frame, len, 0);
    return write(fd, frame, len) == (ssize_t)len ? 0 : -1;
}
int nic_recv(void *buf, size_t max) {
    tick();
    struct pollfd pf = { fd, POLLIN, 0 };
    if (poll(&pf, 1, 1) <= 0) return 0;
    ssize_t n = read(fd, buf, max);
    if (n <= 0) return 0;
    if (loss && rand() % 100 < loss) { dropped_rx++; trace("RX", buf, (size_t)n, 1); return 0; }
    trace("RX", buf, (size_t)n, 0);
    return (int)n;
}
