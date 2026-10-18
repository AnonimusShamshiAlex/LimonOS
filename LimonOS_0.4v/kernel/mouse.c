#include "mouse.h"
#include "io.h"

#define QSIZE 64
static struct mouse_event queue[QSIZE];
static volatile uint32_t qhead, qtail;

static int mx, my, bw = 640, bh = 480;
static uint8_t pkt[3];
static int pi;

void mouse_set_bounds(int w, int h) { bw = w; bh = h; mx = w / 2; my = h / 2; }

void mouse_irq(void) { mouse_feed_byte(inb(0x60)); }

void mouse_feed_byte(uint8_t b) {
    if (pi == 0 && !(b & 8)) return;           /* первый байт пакета всегда с битом 3 — иначе рассинхрон */
    pkt[pi++] = b;
    if (pi < 3) return;
    pi = 0;
    if (pkt[0] & 0xC0) return;                 /* переполнение — пакет выбрасываем */
    int dx = pkt[1] - ((pkt[0] << 4) & 0x100);
    int dy = pkt[2] - ((pkt[0] << 3) & 0x100);
    mx += dx; my -= dy;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= bw) mx = bw - 1;
    if (my >= bh) my = bh - 1;
    uint32_t next = (qhead + 1) % QSIZE;
    if (next == qtail) return;
    queue[qhead].x = mx; queue[qhead].y = my; queue[qhead].buttons = pkt[0] & 7;
    qhead = next;
}

int mouse_poll(struct mouse_event *ev) {
    if (qtail == qhead) return 0;
    *ev = queue[qtail];
    qtail = (qtail + 1) % QSIZE;
    return 1;
}
