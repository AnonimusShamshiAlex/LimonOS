#include "net.h"
#include "nic.h"
#include "timer.h"
#include "kstring.h"

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
