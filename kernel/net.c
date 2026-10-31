#include "net.h"
#include "nic.h"
#include "timer.h"
#include "kstring.h"
#include "format.h"
#include "crypto.h"
#include "mem.h"

static uint8_t txb[1600], rxb[2048];
static struct netinfo ni;
static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static uint16_t csum(const uint8_t *d, size_t n) {
    uint32_t s = 0;
    size_t i = 0;
    for (; i + 1 < n; i += 2) s += (uint32_t)((d[i] << 8) | d[i + 1]);
    if (n & 1) s += (uint32_t)d[n - 1] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

static uint64_t deadline_ms(int ms) { return timer_ticks + (uint64_t)((ms * TIMER_HZ + 999) / 1000); }

/* ---------- Ethernet ---------- */

static int eth_send(const uint8_t *dst, uint16_t type, const uint8_t *pl, size_t len) {
    if (len > 1500) return -1;
    memcpy(txb, dst, 6);
    memcpy(txb + 6, ni.mac, 6);
    wr16(txb + 12, type);
    memcpy(txb + 14, pl, len);
    size_t total = 14 + len;
    if (total < 60) { memset(txb + total, 0, 60 - total); total = 60; }
    return nic_send(txb, total);
}

/* ---------- ARP ---------- */

static struct { uint32_t ip; uint8_t mac[6]; int ok; } arpc[8];
static int arp_next;

static void arp_learn(uint32_t ip, const uint8_t *mac) {
    if (!ip) return;
    for (int i = 0; i < 8; i++)
        if (arpc[i].ok && arpc[i].ip == ip) { memcpy(arpc[i].mac, mac, 6); return; }
    arpc[arp_next].ip = ip; arpc[arp_next].ok = 1; memcpy(arpc[arp_next].mac, mac, 6);
    arp_next = (arp_next + 1) % 8;
}

static int arp_lookup(uint32_t ip, uint8_t *mac) {
    for (int i = 0; i < 8; i++)
        if (arpc[i].ok && arpc[i].ip == ip) { memcpy(mac, arpc[i].mac, 6); return 1; }
    return 0;
}

static void arp_send(uint16_t op, const uint8_t *dmac, uint32_t tip, const uint8_t *tha) {
    uint8_t p[28];
    wr16(p, 1); wr16(p + 2, 0x0800); p[4] = 6; p[5] = 4; wr16(p + 6, op);
    memcpy(p + 8, ni.mac, 6); wr32(p + 14, ni.ip);
    memcpy(p + 18, tha, 6); wr32(p + 24, tip);
    eth_send(dmac, 0x0806, p, 28);
}

static void arp_rx(const uint8_t *p, size_t n) {
    if (n < 28 || rd16(p) != 1 || rd16(p + 2) != 0x0800) return;
    uint16_t op = rd16(p + 6);
    uint32_t sip = rd32(p + 14), tip = rd32(p + 24);
    arp_learn(sip, p + 8);
    if (op == 1 && ni.ip && tip == ni.ip) arp_send(2, p + 8, sip, p + 8);
}

static int arp_resolve(uint32_t ip, uint8_t *mac) {
    static const uint8_t zero[6] = { 0 };
    for (int tries = 0; tries < 3; tries++) {
        if (arp_lookup(ip, mac)) return 1;
        arp_send(1, BCAST, ip, zero);
        uint64_t dl = deadline_ms(300);
        while (timer_ticks < dl) {
            net_poll();
            if (arp_lookup(ip, mac)) return 1;
        }
    }
    return 0;
}

/* ---------- IPv4 ---------- */

static uint16_t ip_id;

static int ip_send(uint32_t dst, uint8_t proto, const uint8_t *pl, size_t len) {
    uint8_t pkt[1500];
    if (len > 1480) return -1;
    pkt[0] = 0x45; pkt[1] = 0;
    wr16(pkt + 2, (uint16_t)(20 + len)); wr16(pkt + 4, ip_id++); wr16(pkt + 6, 0x4000);
    pkt[8] = 64; pkt[9] = proto; wr16(pkt + 10, 0);
    wr32(pkt + 12, ni.ip); wr32(pkt + 16, dst);
    wr16(pkt + 10, csum(pkt, 20));
    memcpy(pkt + 20, pl, len);
    uint8_t mac[6];
    if (dst == 0xFFFFFFFF || !ni.ip) memcpy(mac, BCAST, 6);
    else {
        uint32_t hop = ((dst ^ ni.ip) & ni.mask) == 0 ? dst : ni.gw;
        if (!hop || !arp_resolve(hop, mac)) return -1;
    }
    return eth_send(mac, 0x0800, pkt, 20 + len);
}

/* ---------- ICMP ---------- */

static volatile int ping_got;
static uint16_t ping_seq;
#define PING_ID 0x4C4D

static void icmp_rx(uint32_t src, const uint8_t *p, size_t n) {
    if (n < 8) return;
    if (p[0] == 8) {                                   /* нас пингуют — отвечаем */
        uint8_t r[1480];
        if (n > sizeof r) return;
        memcpy(r, p, n);
        r[0] = 0; wr16(r + 2, 0); wr16(r + 2, csum(r, n));
        ip_send(src, 1, r, n);
    } else if (p[0] == 0 && rd16(p + 4) == PING_ID && rd16(p + 6) == ping_seq) {
        ping_got = 1;
    }
}

int net_ping(uint32_t ip, int timeout_ms, int *rtt_ms) {
    uint8_t p[40];
    if (!ni.configured) return -1;
    p[0] = 8; p[1] = 0; wr16(p + 2, 0); wr16(p + 4, PING_ID); wr16(p + 6, ++ping_seq);
    for (int i = 0; i < 32; i++) p[8 + i] = (uint8_t)(0x61 + i % 23);
    wr16(p + 2, csum(p, 40));
    ping_got = 0;
    uint64_t t0 = timer_ticks;
    if (ip_send(ip, 1, p, 40) < 0) return -1;
    uint64_t dl = deadline_ms(timeout_ms);
    while (timer_ticks < dl) {
        net_poll();
        if (ping_got) { *rtt_ms = (int)((timer_ticks - t0) * 1000 / TIMER_HZ); return 0; }
    }
    return -2;
}

/* ---------- DHCP и DNS (поверх UDP) ---------- */

static int udp_send(uint32_t dst, uint16_t sp, uint16_t dp, const uint8_t *d, size_t n) {
    uint8_t b[1480];
    if (n + 8 > sizeof b) return -1;
    wr16(b, sp); wr16(b + 2, dp); wr16(b + 4, (uint16_t)(8 + n)); wr16(b + 6, 0);
    memcpy(b + 8, d, n);
    return ip_send(dst, 17, b, 8 + n);
}

static uint32_t dhcp_xid, d_ip, d_mask, d_gw, d_dns, d_srv;
static volatile int dhcp_stage;     /* 1 — DISCOVER отправлен, 2 — получен OFFER, 3 — REQUEST отправлен, 4 — получен ACK */

static size_t dhcp_build(uint8_t *b, uint8_t type) {
    memset(b, 0, 300);
    b[0] = 1; b[1] = 1; b[2] = 6;
    wr32(b + 4, dhcp_xid); wr16(b + 10, 0x8000);
    memcpy(b + 28, ni.mac, 6);
    b[236] = 99; b[237] = 130; b[238] = 83; b[239] = 99;
    uint8_t *o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    if (type == 3) {
        *o++ = 50; *o++ = 4; wr32(o, d_ip); o += 4;
        *o++ = 54; *o++ = 4; wr32(o, d_srv); o += 4;
    }
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 15;
    *o++ = 255;
    return 300;
}

static void dhcp_rx(const uint8_t *d, size_t n) {
    if (n < 241 || d[0] != 2 || rd32(d + 4) != dhcp_xid || d[236] != 99 || d[237] != 130) return;
    if (memcmp(d + 28, ni.mac, 6) != 0) return;
    int type = 0;
    uint32_t mask = 0, gw = 0, dns = 0, srv = 0;
    for (size_t o = 240; o + 1 < n;) {
        uint8_t code = d[o];
        if (code == 255) break;
        if (code == 0) { o++; continue; }
        uint8_t len = d[o + 1];
        if (o + 2 + len > n) break;
        const uint8_t *v = d + o + 2;
        switch (code) {
        case 53: type = v[0]; break;
        case 1:  if (len >= 4) mask = rd32(v); break;
        case 3:  if (len >= 4) gw = rd32(v); break;
        case 6:  if (len >= 4) dns = rd32(v); break;
        case 54: if (len >= 4) srv = rd32(v); break;
        default: break;
        }
        o += 2 + (size_t)len;
    }
    if (type == 2 && dhcp_stage == 1) {
        d_ip = rd32(d + 16); d_mask = mask; d_gw = gw; d_dns = dns; d_srv = srv; dhcp_stage = 2;
    } else if (type == 5 && dhcp_stage == 3) {
        d_ip = rd32(d + 16);
        if (mask) d_mask = mask;
        if (gw) d_gw = gw;
        if (dns) d_dns = dns;
        dhcp_stage = 4;
    }
}

int net_dhcp(void) {
    uint8_t b[300];
    if (!ni.present) return -1;
    ni.ip = 0; ni.mask = 0; ni.gw = 0; ni.dns = 0; ni.configured = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        dhcp_xid = 0x4C494D4Fu ^ (uint32_t)timer_ticks ^ (uint32_t)attempt;
        dhcp_stage = 1;
        udp_send(0xFFFFFFFF, 68, 67, b, dhcp_build(b, 1));
        uint64_t dl = deadline_ms(2000);
        while (timer_ticks < dl && dhcp_stage != 2) net_poll();
        if (dhcp_stage != 2) continue;
        dhcp_stage = 3;
        udp_send(0xFFFFFFFF, 68, 67, b, dhcp_build(b, 3));
        dl = deadline_ms(2000);
        while (timer_ticks < dl && dhcp_stage != 4) net_poll();
        if (dhcp_stage != 4) continue;
        ni.ip = d_ip; ni.mask = d_mask ? d_mask : 0xFFFFFF00; ni.gw = d_gw; ni.dns = d_dns ? d_dns : d_gw;
        ni.configured = 1;
        return 0;
    }
    return -2;
}

static uint16_t dns_id, dns_sport;
static volatile int dns_state;      /* 1 — ответ получен, -1 — ошибка */
static uint32_t dns_ip;

static const uint8_t *skip_name(const uint8_t *p, const uint8_t *end) {
    while (p < end) {
        uint8_t l = *p;
        if (l == 0) return p + 1;
        if ((l & 0xC0) == 0xC0) return p + 2 <= end ? p + 2 : 0;
        p += (size_t)l + 1;
    }
    return 0;
}

static void dns_rx(const uint8_t *d, size_t n) {
    if (n < 12 || rd16(d) != dns_id) return;
    uint16_t fl = rd16(d + 2);
    if (!(fl & 0x8000)) return;
    if ((fl & 0xF) != 0) { dns_state = -1; return; }
    uint16_t qd = rd16(d + 4), an = rd16(d + 6);
    const uint8_t *p = d + 12, *end = d + n;
    for (int i = 0; i < qd; i++) {
        p = skip_name(p, end);
        if (!p || p + 4 > end) { dns_state = -1; return; }
        p += 4;
    }
    for (int i = 0; i < an; i++) {
        p = skip_name(p, end);
        if (!p || p + 10 > end) break;
        uint16_t t = rd16(p), rl = rd16(p + 8);
        p += 10;
        if (p + rl > end) break;
        if (t == 1 && rl == 4) { dns_ip = rd32(p); dns_state = 1; return; }
        p += rl;
    }
    dns_state = -1;
}

static void udp_rx(const uint8_t *p, size_t n) {
    if (n < 8) return;
    uint16_t dp = rd16(p + 2), len = rd16(p + 4);
    if (len < 8 || len > n) return;
    if (dp == 68) dhcp_rx(p + 8, len - 8);
    else if (dp == dns_sport) dns_rx(p + 8, len - 8);
}

int net_parse_ip(const char *s, uint32_t *ip) {
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return -1;
        uint32_t o = 0;
        while (*s >= '0' && *s <= '9') { o = o * 10 + (uint32_t)(*s++ - '0'); if (o > 255) return -1; }
        v = (v << 8) | o;
        if (part < 3) { if (*s != '.') return -1; s++; }
    }
    if (*s) return -1;
    *ip = v;
    return 0;
}

void net_ip_str(uint32_t ip, char out[16]) {
    int n = 0;
    for (int i = 3; i >= 0; i--) {
        unsigned v = (ip >> (i * 8)) & 255;
        char t[3]; int k = 0;
        do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (k) out[n++] = t[--k];
        if (i) out[n++] = '.';
    }
    out[n] = 0;
}

int net_resolve(const char *host, uint32_t *ip_out) {
    if (net_parse_ip(host, ip_out) == 0) return 0;
    if (!ni.configured || !ni.dns) return -1;
    size_t hl = strlen(host);
    if (hl == 0 || hl > 200) return -1;
    static uint16_t counter;
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t q[300];
        counter++;
        dns_id = (uint16_t)(0x4C00 + counter);
        dns_sport = (uint16_t)(49152 + (counter & 0xFF));
        memset(q, 0, 12);
        wr16(q, dns_id); wr16(q + 2, 0x0100); wr16(q + 4, 1);
        size_t o = 12;
        const char *p = host;
        while (*p) {
            const char *dot = p;
            while (*dot && *dot != '.') dot++;
            size_t l = (size_t)(dot - p);
            if (l == 0 || l > 63) return -1;
            q[o++] = (uint8_t)l;
            memcpy(q + o, p, l); o += l;
            p = *dot ? dot + 1 : dot;
        }
        q[o++] = 0;
        wr16(q + o, 1); wr16(q + o + 2, 1); o += 4;
        dns_state = 0;
        if (udp_send(ni.dns, dns_sport, 53, q, o) < 0) continue;
        uint64_t dl = deadline_ms(2000);
        while (timer_ticks < dl && dns_state == 0) net_poll();
        if (dns_state == 1) { *ip_out = dns_ip; return 0; }
        if (dns_state == -1) return -3;
    }
    return -2;
}

/* ---------- TCP ---------- */
/* Надёжный клиент: приёмное кольцо 64 КБ с честным окном, буфер отправки с
   повтором потерянных сегментов (таймер RTO с удвоением), повторный ACK при
   нарушении порядка (чтобы сервер быстро переслал потерянное), проверка
   контрольной суммы, MSS. Без прерываний: всё крутится внутри net_poll(). */

#define TCP_MAX     6
#define TCP_RX_CAP  65536u
#define TCP_TX_CAP  16384u
#define TCP_MSS     1460u
#define TCP_RTO_MIN 300          /* мс */
#define TCP_RTO_MAX 6000
#define TCP_RETRIES 9

enum { TCP_CLOSED = 0, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT };

struct tcp_conn {
    int state;
    uint32_t rip;
    uint16_t rport, lport;
    uint32_t snd_una, snd_nxt, rcv_nxt;
    uint16_t snd_wnd, mss;
    uint8_t *rx; uint32_t rx_head, rx_len;
    uint8_t *tx; uint32_t tx_len;                  /* байты, начиная с snd_una, ещё не подтверждённые или не отправленные */
    uint64_t rto_at; uint32_t rto_ms; int retries;
    int got_fin, got_rst, want_fin, fin_sent, fin_acked, ack_pending;
    struct { uint32_t seq, len; } ooo[4];          /* куски, пришедшие не по порядку (лежат прямо в кольце) */
    int dupacks;
    uint32_t srtt, rttvar, rto_base;               /* оценка времени отклика, мс */
    uint32_t rtt_seq; uint64_t rtt_t0; int rtt_on;
    int sack_ok;                                   /* собеседник согласился на SACK */
};
static struct tcp_conn tcpc[TCP_MAX];

static uint16_t tcp_csum(uint32_t src, uint32_t dst, const uint8_t *seg, size_t n) {
    uint32_t s = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + 6 + (uint32_t)n;
    size_t i = 0;
    for (; i + 1 < n; i += 2) s += (uint32_t)((seg[i] << 8) | seg[i + 1]);
    if (n & 1) s += (uint32_t)seg[n - 1] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

static uint16_t tcp_adv_wnd(const struct tcp_conn *c) {
    uint32_t f = c->rx ? TCP_RX_CAP - c->rx_len : 0;
    return (uint16_t)(f > 65535 ? 65535 : f);
}

static int tcp_seg(struct tcp_conn *c, uint8_t flags, uint32_t seq, const uint8_t *data, size_t dlen) {
    uint8_t seg[60 + TCP_MSS];
    size_t hl = 20;
    if (dlen > TCP_MSS) return -1;
    wr16(seg, c->lport); wr16(seg + 2, c->rport);
    wr32(seg + 4, seq); wr32(seg + 8, (flags & 0x10) ? c->rcv_nxt : 0);
    seg[13] = flags;
    wr16(seg + 14, tcp_adv_wnd(c)); wr16(seg + 16, 0); wr16(seg + 18, 0);
    if (flags & 0x02) {                                  /* SYN: MSS + разрешение SACK */
        seg[20] = 2; seg[21] = 4; wr16(seg + 22, (uint16_t)TCP_MSS);
        seg[24] = 1; seg[25] = 1; seg[26] = 4; seg[27] = 2;
        hl = 28;
    } else if (c->sack_ok && dlen == 0 && (flags & 0x10)) {   /* повторный ACK: перечисляем куски, которые у нас уже есть */
        int n = 0;
        for (int i = 0; i < 4; i++) if (c->ooo[i].len) n++;
        if (n) {
            seg[20] = 1; seg[21] = 1; seg[22] = 5; seg[23] = (uint8_t)(2 + 8 * n);
            size_t o = 24;
            for (int i = 0; i < 4; i++) if (c->ooo[i].len) { wr32(seg + o, c->ooo[i].seq); wr32(seg + o + 4, c->ooo[i].seq + c->ooo[i].len); o += 8; }
            hl = o;
        }
    }
    seg[12] = (uint8_t)((hl / 4) << 4);
    if (dlen) memcpy(seg + hl, data, dlen);
    wr16(seg + 16, tcp_csum(ni.ip, c->rip, seg, hl + dlen));
    return ip_send(c->rip, 6, seg, hl + dlen);
}

static void tcp_send_ack(struct tcp_conn *c) {
    c->ack_pending = 0;
    tcp_seg(c, 0x10, c->snd_nxt, NULL, 0);
}

static void tcp_free(struct tcp_conn *c) {
    if (c->rx) kfree(c->rx);
    if (c->tx) kfree(c->tx);
    c->rx = c->tx = NULL;
}

static void tcp_arm(struct tcp_conn *c) { c->rto_at = timer_ticks + (uint64_t)((c->rto_ms * TIMER_HZ + 999) / 1000); }

/* отправить всё, что позволяет окно получателя */
static void tcp_flush(struct tcp_conn *c) {
    if (c->state != TCP_ESTABLISHED && c->state != TCP_FIN_WAIT) return;
    uint32_t wnd = c->snd_wnd < TCP_TX_CAP ? c->snd_wnd : TCP_TX_CAP;
    for (;;) {
        uint32_t sent = c->fin_sent ? c->tx_len : c->snd_nxt - c->snd_una;
        if (sent >= c->tx_len) break;
        if (sent >= wnd) break;
        uint32_t k = c->tx_len - sent;
        if (k > c->mss) k = c->mss;
        if (k > wnd - sent) k = wnd - sent;
        if (!k) break;
        if (tcp_seg(c, 0x18, c->snd_nxt, c->tx + sent, k) < 0) break;
        if (!c->rtt_on) { c->rtt_on = 1; c->rtt_seq = c->snd_nxt + k; c->rtt_t0 = timer_ticks; }
        c->snd_nxt += k;
        if (!c->rto_at) tcp_arm(c);
    }
    uint32_t sent = c->fin_sent ? c->tx_len : c->snd_nxt - c->snd_una;
    if (c->want_fin && !c->fin_sent && sent >= c->tx_len) {
        tcp_seg(c, 0x11, c->snd_nxt, NULL, 0);
        c->fin_sent = 1; c->snd_nxt++; c->state = TCP_FIN_WAIT;
        if (!c->rto_at) tcp_arm(c);
    }
}

static void tcp_timers(void) {
    for (int i = 0; i < TCP_MAX; i++) {
        struct tcp_conn *c = &tcpc[i];
        if ((c->state != TCP_ESTABLISHED && c->state != TCP_FIN_WAIT) || !c->rto_at || timer_ticks < c->rto_at) continue;
        if (++c->retries > TCP_RETRIES) { c->state = TCP_CLOSED; c->got_rst = 1; c->rto_at = 0; continue; }
        c->rto_ms = c->rto_ms * 2 > TCP_RTO_MAX ? TCP_RTO_MAX : c->rto_ms * 2;
        c->rtt_on = 0;
        /* начинаем заново с первого неподтверждённого байта (go-back-N) */
        c->snd_nxt = c->snd_una;
        c->fin_sent = 0;
        c->rto_at = 0;
        tcp_flush(c);
        if (c->snd_nxt == c->snd_una && !c->want_fin) {   /* нечего слать: зонд окна */
            tcp_send_ack(c);
        }
        if (!c->rto_at) tcp_arm(c);
    }
}

static void tcp_rx(uint32_t src, const uint8_t *p, size_t n) {
    if (n < 20) return;
    uint16_t sport = rd16(p), dport = rd16(p + 2);
    uint32_t seq = rd32(p + 4), ackn = rd32(p + 8);
    size_t doff = (size_t)(p[12] >> 4) * 4;
    uint8_t flags = p[13];
    if (doff < 20 || doff > n) return;
    if (tcp_csum(src, ni.ip, p, n) != 0) return;           /* повреждённый сегмент */
    const uint8_t *data = p + doff;
    size_t dlen = n - doff;

    struct tcp_conn *c = NULL;
    for (int i = 0; i < TCP_MAX; i++)
        if (tcpc[i].state != TCP_CLOSED && tcpc[i].rip == src && tcpc[i].rport == sport && tcpc[i].lport == dport)
            { c = &tcpc[i]; break; }
    if (!c) return;
    if (flags & 0x04) { c->state = TCP_CLOSED; c->got_rst = 1; c->rto_at = 0; return; }   /* RST */

    if (c->state == TCP_SYN_SENT) {
        if ((flags & 0x12) == 0x12 && ackn == c->snd_nxt + 1) {                 /* SYN+ACK */
            c->rcv_nxt = seq + 1;
            c->snd_nxt += 1; c->snd_una = c->snd_nxt;
            c->snd_wnd = rd16(p + 14);
            for (size_t o = 20; o + 4 <= doff; ) {                               /* ищем MSS */
                if (p[o] == 0) break;
                if (p[o] == 1) { o++; continue; }
                if (o + 1 >= doff || p[o + 1] < 2) break;
                if (p[o] == 2 && p[o + 1] == 4) { uint16_t m = rd16(p + o + 2); if (m && m < c->mss) c->mss = m; }
                if (p[o] == 4 && p[o + 1] == 2) c->sack_ok = 1;
                o += p[o + 1];
            }
            c->state = TCP_ESTABLISHED;
            tcp_send_ack(c);
        }
        return;
    }
    if (c->state != TCP_ESTABLISHED && c->state != TCP_FIN_WAIT) return;

    /* --- подтверждения нашим данным --- */
    if (flags & 0x10) {
        uint32_t acked = ackn - c->snd_una, outstanding = c->snd_nxt - c->snd_una;
        if (acked > 0 && acked <= outstanding) {
            uint32_t da = acked;
            if (c->fin_sent && acked == outstanding) { c->fin_acked = 1; da = acked - 1; }
            if (da > c->tx_len) da = c->tx_len;
            if (da) { memmove(c->tx, c->tx + da, c->tx_len - da); c->tx_len -= da; }
            c->snd_una = ackn;
            c->retries = 0; c->dupacks = 0;
            if (c->rtt_on && (int32_t)(ackn - c->rtt_seq) >= 0) {           /* пришло подтверждение замеряемого сегмента */
                uint32_t sample = (uint32_t)((timer_ticks - c->rtt_t0) * (1000 / TIMER_HZ));
                if (!sample) sample = 5;
                if (!c->srtt) { c->srtt = sample; c->rttvar = sample / 2; }
                else {
                    uint32_t diff = c->srtt > sample ? c->srtt - sample : sample - c->srtt;
                    c->rttvar = (3 * c->rttvar + diff) / 4;
                    c->srtt = (7 * c->srtt + sample) / 8;
                }
                uint32_t rto = c->srtt + 4 * c->rttvar;
                c->rto_base = rto < TCP_RTO_MIN ? TCP_RTO_MIN : (rto > TCP_RTO_MAX ? TCP_RTO_MAX : rto);
                c->rtt_on = 0;
            }
            c->rto_ms = c->rto_base;
            c->rto_at = 0;
            if (c->snd_una != c->snd_nxt) tcp_arm(c);
        } else if (acked == 0 && outstanding > 0 && dlen == 0 && !(flags & 0x03) && rd16(p + 14) == c->snd_wnd) {
            if (++c->dupacks == 3) {                                         /* три одинаковых ACK подряд: сегмент потерян — шлём заново, не дожидаясь таймера */
                c->snd_nxt = c->snd_una; c->fin_sent = 0; c->rtt_on = 0;
                tcp_flush(c);
            }
        }
        c->snd_wnd = rd16(p + 14);
        tcp_flush(c);
    }

    /* --- входящие данные --- */
    if (dlen == 0 && !(flags & 0x01)) return;
    if ((int32_t)(seq - c->rcv_nxt) < 0) {                  /* часть или всё уже получено — отрезаем старое */
        uint32_t skip = c->rcv_nxt - seq;
        if (skip >= dlen + ((flags & 0x01) ? 1 : 0)) { tcp_send_ack(c); return; }
        if (skip <= dlen) { data += skip; dlen -= skip; seq = c->rcv_nxt; }
    }
    uint32_t freeb = TCP_RX_CAP - c->rx_len;
    if (seq != c->rcv_nxt) {                                 /* дыра: кладём кусок на своё место в кольце и сразу шлём повторный ACK */
        uint32_t off = seq - c->rcv_nxt;
        if (dlen && (int32_t)off > 0 && off + dlen <= freeb) {
            uint32_t pos = (c->rx_head + c->rx_len + off) % TCP_RX_CAP;
            uint32_t first = TCP_RX_CAP - pos; if (first > dlen) first = (uint32_t)dlen;
            memcpy(c->rx + pos, data, first);
            if (dlen > first) memcpy(c->rx, data + first, dlen - first);
            /* запоминаем диапазон; склеиваем пересекающиеся */
            uint32_t s0 = seq, e0 = seq + (uint32_t)dlen;
            for (int i = 0; i < 4; i++) {
                if (!c->ooo[i].len) continue;
                uint32_t s1 = c->ooo[i].seq, e1 = s1 + c->ooo[i].len;
                if ((int32_t)(s0 - e1) <= 0 && (int32_t)(s1 - e0) <= 0) {
                    if ((int32_t)(s1 - s0) < 0) s0 = s1;
                    if ((int32_t)(e1 - e0) > 0) e0 = e1;
                    c->ooo[i].len = 0;
                }
            }
            for (int i = 0; i < 4; i++) if (!c->ooo[i].len) { c->ooo[i].seq = s0; c->ooo[i].len = e0 - s0; break; }
        }
        tcp_send_ack(c);
        return;
    }
    if (dlen) {
        if (dlen > freeb) { tcp_send_ack(c); return; }       /* окно нарушено — выбросим, пусть повторит */
        uint32_t tail = (c->rx_head + c->rx_len) % TCP_RX_CAP;
        uint32_t first = TCP_RX_CAP - tail; if (first > dlen) first = (uint32_t)dlen;
        memcpy(c->rx + tail, data, first);
        if (dlen > first) memcpy(c->rx, data + first, dlen - first);
        c->rx_len += (uint32_t)dlen;
        c->rcv_nxt += (uint32_t)dlen;
        c->ack_pending = 1;
        for (int again = 1; again; ) {                       /* дыра закрыта — присоединяем уже лежащие куски */
            again = 0;
            for (int i = 0; i < 4; i++) {
                if (!c->ooo[i].len) continue;
                uint32_t s1 = c->ooo[i].seq, e1 = s1 + c->ooo[i].len;
                if ((int32_t)(s1 - c->rcv_nxt) <= 0) {
                    if ((int32_t)(e1 - c->rcv_nxt) > 0) { uint32_t adv = e1 - c->rcv_nxt; c->rx_len += adv; c->rcv_nxt += adv; }
                    c->ooo[i].len = 0; again = 1;
                }
            }
        }
        if (c->got_fin == 0 && c->ooo[0].len + c->ooo[1].len + c->ooo[2].len + c->ooo[3].len) c->ack_pending = 1;
    }
    if (flags & 0x01) {
        int gap = 0; for (int i = 0; i < 4; i++) if (c->ooo[i].len) gap = 1;
        if (!gap) { c->rcv_nxt += 1; c->got_fin = 1; tcp_send_ack(c); }
        else tcp_send_ack(c);
    }
}

int tcp_connect(uint32_t ip, uint16_t port, int timeout_ms) {
    int i = -1;
    for (int k = 0; k < TCP_MAX; k++) if (tcpc[k].state == TCP_CLOSED) { i = k; break; }
    if (i < 0) return -1;
    struct tcp_conn *c = &tcpc[i];
    tcp_free(c);
    memset(c, 0, sizeof *c);
    c->rx = kmalloc(TCP_RX_CAP); c->tx = kmalloc(TCP_TX_CAP);
    if (!c->rx || !c->tx) { tcp_free(c); return -4; }
    uint32_t iss; uint16_t lp;
    rng_bytes(&iss, 4); rng_bytes(&lp, 2);
    c->rip = ip; c->rport = port;
    c->lport = (uint16_t)(0xC000 | (lp & 0x3FFF));
    c->snd_nxt = c->snd_una = iss;
    c->mss = TCP_MSS; c->rto_base = 1000; c->rto_ms = 1000; c->snd_wnd = 1024;
    c->state = TCP_SYN_SENT;
    int per_try = timeout_ms / 4; if (per_try < 400) per_try = 400;
    for (int attempt = 0; attempt < 4 && c->state == TCP_SYN_SENT; attempt++) {
        tcp_seg(c, 0x02, c->snd_nxt, NULL, 0);
        uint64_t dl = deadline_ms(per_try);
        while (timer_ticks < dl && c->state == TCP_SYN_SENT) net_poll();
        if (c->got_rst) break;                              /* порт закрыт — отказ пришёл сразу, незачем повторять SYN */
        per_try *= 2;
    }
    if (c->state != TCP_ESTABLISHED) {
        int rst = c->got_rst;
        c->state = TCP_CLOSED; tcp_free(c);
        return rst ? -3 : -2;                                /* -3 порт закрыт (RST), -2 не отвечает (фильтр/занято) */
    }
    return i;
}

int tcp_send(int id, const void *data, size_t len) {
    if (id < 0 || id >= TCP_MAX) return -1;
    struct tcp_conn *c = &tcpc[id];
    const uint8_t *d = data;
    while (len) {
        if ((c->state != TCP_ESTABLISHED) || c->want_fin) return c->got_rst ? -3 : -2;
        uint32_t room = TCP_TX_CAP - c->tx_len;
        if (!room) { net_poll(); continue; }               /* ждём подтверждений; при обрыве сработает счётчик повторов */
        uint32_t k = len < room ? (uint32_t)len : room;
        memcpy(c->tx + c->tx_len, d, k);
        c->tx_len += k; d += k; len -= k;
        tcp_flush(c);
    }
    return 0;
}

/* >0 — байты, 0 — таймаут, -1 — собеседник закрыл (данных больше нет), -2 — обрыв/сброс */
int tcp_recv(int id, void *buf, size_t max, int timeout_ms) {
    if (id < 0 || id >= TCP_MAX) return -2;
    struct tcp_conn *c = &tcpc[id];
    uint64_t dl = deadline_ms(timeout_ms);
    for (;;) {
        if (c->rx_len) {
            uint32_t before = TCP_RX_CAP - c->rx_len;
            size_t k = max < c->rx_len ? max : c->rx_len;
            size_t first = TCP_RX_CAP - c->rx_head; if (first > k) first = k;
            memcpy(buf, c->rx + c->rx_head, first);
            if (k > first) memcpy((uint8_t *)buf + first, c->rx, k - first);
            c->rx_head = (c->rx_head + (uint32_t)k) % TCP_RX_CAP;
            c->rx_len -= (uint32_t)k;
            if (before < 2 * TCP_MSS && c->state != TCP_CLOSED) tcp_send_ack(c);    /* окно было почти закрыто — сообщаем, что место есть */
            return (int)k;
        }
        if (c->got_fin) return -1;
        if (c->state == TCP_CLOSED) return -2;
        if (timer_ticks >= dl) return 0;
        net_poll();
    }
}

void tcp_close(int id) {
    if (id < 0 || id >= TCP_MAX) return;
    struct tcp_conn *c = &tcpc[id];
    if (c->state == TCP_ESTABLISHED) {
        c->want_fin = 1;
        tcp_flush(c);
        uint64_t dl = deadline_ms(3000);
        while (timer_ticks < dl && c->state != TCP_CLOSED && !c->fin_acked) net_poll();
    }
    c->state = TCP_CLOSED;
    tcp_free(c);
}

/* информация для команды netstat */
int tcp_info(int id, uint32_t *ip, uint16_t *port, int *state, uint32_t *queued) {
    if (id < 0 || id >= TCP_MAX || tcpc[id].state == TCP_CLOSED) return -1;
    *ip = tcpc[id].rip; *port = tcpc[id].rport; *state = tcpc[id].state; *queued = tcpc[id].rx_len;
    return 0;
}

int http_header(const char *r, size_t headers_len, const char *name, char *out, size_t outmax) {
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl + 1 < headers_len; i++) {
        if (r[i] == '\n') {
            size_t s = i + 1, j = 0;
            for (; j < nl && s + j < headers_len; j++) {
                char a = r[s + j]; if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                char b = name[j]; if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                if (a != b) break;
            }
            if (j == nl && s + j < headers_len && r[s + j] == ':') {
                size_t v = s + j + 1;
                while (v < headers_len && r[v] == ' ') v++;
                size_t o = 0;
                while (v < headers_len && r[v] != '\r' && r[v] != '\n' && o + 1 < outmax) out[o++] = r[v++];
                out[o] = 0;
                return 0;
            }
        }
    }
    return -1;
}

/* ---------- приём ---------- */

static void ip_rx(const uint8_t *p, size_t n) {
    if (n < 20 || (p[0] >> 4) != 4) return;
    size_t ihl = (size_t)(p[0] & 15) * 4;
    if (ihl < 20 || n < ihl) return;
    size_t tl = rd16(p + 2);
    if (tl < ihl || tl > n) return;
    if (csum(p, ihl) != 0) return;
    uint32_t dst = rd32(p + 16);
    if (ni.ip && dst != ni.ip && dst != 0xFFFFFFFF) return;
    if (rd16(p + 6) & 0x3FFF) return;                  /* фрагменты пока не собираем */
    uint32_t src = rd32(p + 12);
    const uint8_t *pl = p + ihl;
    size_t len = tl - ihl;
    if (p[9] == 1) icmp_rx(src, pl, len);
    else if (p[9] == 17) udp_rx(pl, len);
    else if (p[9] == 6) tcp_rx(src, pl, len);
}

void net_poll(void) {
    if (!ni.present) return;
    for (int i = 0; i < 16; i++) {
        int n = nic_recv(rxb, sizeof rxb);
        if (n < 14) break;
        uint16_t type = rd16(rxb + 12);
        if (type == 0x0806) arp_rx(rxb + 14, (size_t)n - 14);
        else if (type == 0x0800) ip_rx(rxb + 14, (size_t)n - 14);
    }
    for (int i = 0; i < TCP_MAX; i++) if (tcpc[i].ack_pending && tcpc[i].state != TCP_CLOSED) tcp_send_ack(&tcpc[i]);
    tcp_timers();
}

int net_init(void) {
    if (nic_init() != 0) return -1;
    ni.present = 1;
    memcpy(ni.mac, nic_mac(), 6);
    ni.nic = nic_name();
    return 0;
}

const struct netinfo *net_info(void) {
    ni.link = ni.present ? nic_link() : 0;
    return &ni;
}

void net_set_static(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns) {
    ni.ip = ip; ni.mask = mask; ni.gw = gw; ni.dns = dns; ni.configured = 1;
}

#ifdef LIMON_HOSTTEST
#include <stdio.h>
void tcp_debug_dump(void) {
    for (int i = 0; i < TCP_MAX; i++) {
        struct tcp_conn *c = &tcpc[i];
        if (c->state == TCP_CLOSED) continue;
        fprintf(stderr, "[tcp %d] state=%d rx_len=%u rcv_nxt=%u snd_una=%u snd_nxt=%u wnd=%u tx_len=%u got_fin=%d retries=%d rto=%u srtt=%u ooo=%u/%u,%u/%u,%u/%u,%u/%u\n",
            i, c->state, c->rx_len, c->rcv_nxt, c->snd_una, c->snd_nxt, c->snd_wnd, c->tx_len, c->got_fin, c->retries, c->rto_ms, c->srtt,
            c->ooo[0].seq - c->rcv_nxt, c->ooo[0].len, c->ooo[1].seq - c->rcv_nxt, c->ooo[1].len, c->ooo[2].seq - c->rcv_nxt, c->ooo[2].len, c->ooo[3].seq - c->rcv_nxt, c->ooo[3].len);
    }
}
#endif

/* ---------- net_url_join: относительные ссылки в браузере Citrus ---------- */

struct joinurl { int tls; uint16_t port; char host[64]; char path[640]; };

static int joinurl_parse(const char *s, struct joinurl *u) {
    memset(u, 0, sizeof *u);
    u->port = 80;
    if (strncmp(s, "https://", 8) == 0) { u->tls = 1; u->port = 443; s += 8; }
    else if (strncmp(s, "http://", 7) == 0) s += 7;
    size_t h = 0;
    while (*s && *s != '/' && *s != ':' && h < sizeof u->host - 1) u->host[h++] = *s++;
    u->host[h] = 0;
    if (h == 0) return -1;
    if (*s == ':') { s++; unsigned p = 0; while (*s >= '0' && *s <= '9') p = p * 10 + (unsigned)(*s++ - '0'); if (p) u->port = (uint16_t)p; }
    if (*s != '/') { u->path[0] = '/'; u->path[1] = 0; }
    else { size_t n = 0; while (*s && n < sizeof u->path - 1) u->path[n++] = *s++; u->path[n] = 0; }
    return 0;
}

static void joinurl_str(const struct joinurl *u, char *out, size_t max) {
    int def = u->tls ? 443 : 80;
    if (u->port == def) ksnprintf(out, max, "%s://%s%s", u->tls ? "https" : "http", u->host, u->path);
    else ksnprintf(out, max, "%s://%s:%d%s", u->tls ? "https" : "http", u->host, (int)u->port, u->path);
}

static int joinurl_apply(struct joinurl *u, const char *loc) {
    if (strncmp(loc, "http://", 7) == 0 || strncmp(loc, "https://", 8) == 0) return joinurl_parse(loc, u);
    if (loc[0] == '/' && loc[1] == '/') {
        char tmp[700]; ksnprintf(tmp, sizeof tmp, "%s:%s", u->tls ? "https" : "http", loc);
        return joinurl_parse(tmp, u);
    }
    if (loc[0] == '/') { size_t n = 0; while (loc[n] && n < sizeof u->path - 1) { u->path[n] = loc[n]; n++; } u->path[n] = 0; return 0; }
    /* относительный путь: заменяем последний сегмент */
    size_t last = 0; for (size_t i = 0; u->path[i]; i++) if (u->path[i] == '/') last = i;
    size_t n = last + 1;
    for (size_t i = 0; loc[i] && n < sizeof u->path - 1; i++) u->path[n++] = loc[i];
    u->path[n] = 0;
    return 0;
}

int net_url_join(const char *base, const char *ref, char *out, size_t max) {
    struct joinurl u;
    if (joinurl_parse(base, &u) != 0) return -1;
    char r[640]; size_t k = 0;
    while (ref[k] && ref[k] != '#' && k < sizeof r - 1) { r[k] = ref[k]; k++; }
    r[k] = 0;
    if (r[0] == '?') {                                    /* только запрос: путь тот же */
        size_t p = 0; while (u.path[p] && u.path[p] != '?') p++;
        size_t j = 0; while (r[j] && p + j < sizeof u.path - 1) { u.path[p + j] = r[j]; j++; }
        u.path[p + j] = 0;
    } else if (r[0] && joinurl_apply(&u, r) != 0) return -1;
    joinurl_str(&u, out, max);
    return 0;
}
