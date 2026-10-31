#include "tls.h"
#include "crypto.h"
#include "kstring.h"
#include "mem.h"

#define REC_MAX   16384
#define REC_BUF   (5 + REC_MAX + 256)
#define HS_CAP    32768
#define IO_WAIT   8000      /* мс ожидания одного куска во время рукопожатия */

enum { SUITE_CHACHA = 0x1303, SUITE_AES128 = 0x1301 };

struct dir {
    uint8_t secret[32], key[32], iv[12];
    uint64_t seq;
    int on;                 /* 1 — записи этого направления шифруются */
    int pend;               /* TLS 1.2: ключи готовы, включатся после ChangeCipherSpec */
};

struct tls {
    struct tls_io io;
    int suite, keylen;
    struct dir rx, tx;
    struct sha256 th;       /* стенограмма рукопожатия */
    uint8_t *rbuf, *wbuf;   /* буферы записи (приём/отправка) */
    size_t rfill;           /* сколько байт текущей записи уже получено */
    size_t pt_off, pt_len;  /* непрочитанные данные приложения в rbuf+5 */
    int pt_valid;
    int closed, err;
    /* рукопожатие */
    uint8_t *hs; size_t hs_len, hs_used;
    struct tls_info info;
    /* TLS 1.2 */
    int v12, ems, flags;
    uint8_t cr[32], sr[32], master[48];
    uint8_t sid[32]; uint8_t sidlen;
};

const char *tls_strerror(int e) {
    switch (e) {
    case TLS_OK: return "ok";
    case TLS_E_IO: return "соединение оборвалось";
    case TLS_E_PROTO: return "сервер нарушил протокол TLS";
    case TLS_E_CRYPTO: return "проверка подлинности данных не прошла";
    case TLS_E_ALERT: return "сервер отклонил соединение (alert)";
    case TLS_E_UNSUPPORTED: return "сервер требует то, что мы не умеем";
    case TLS_EOF: return "сервер закрыл соединение";
    case TLS_E_MEM: return "не хватило памяти";
    case TLS_E_TIMEOUT: return "сервер не отвечает";
    }
    return "неизвестная ошибка";
}

const struct tls_info *tls_info(const struct tls *t) { return &t->info; }

/* ---------- мелочи ---------- */

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put24(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }
static unsigned get16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static unsigned get24(const uint8_t *p) { return ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2]; }

static void th_snapshot(const struct tls *t, uint8_t out[32]) {
    struct sha256 c = t->th;
    sha256_final(&c, out);
}

static void dir_set_keys(struct tls *t, struct dir *d, const uint8_t secret[32]) {
    memcpy(d->secret, secret, 32);
    tls13_expand_label(secret, "key", NULL, 0, d->key, (size_t)t->keylen);
    tls13_expand_label(secret, "iv", NULL, 0, d->iv, 12);
    d->seq = 0;
    d->on = 1;
}

static void make_nonce(const struct dir *d, uint8_t nonce[12]) {
    memcpy(nonce, d->iv, 12);
    for (int i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(d->seq >> (8 * i));
}

static void aead_seal(const struct tls *t, const struct dir *d, const uint8_t *aad, const uint8_t *pt, size_t n, uint8_t *ct, uint8_t *tag) {
    uint8_t nonce[12];
    make_nonce(d, nonce);
    if (t->suite == SUITE_CHACHA) chacha20poly1305_seal(d->key, nonce, aad, 5, pt, n, ct, tag);
    else                          aes128gcm_seal(d->key, nonce, aad, 5, pt, n, ct, tag);
}

static int aead_open(const struct tls *t, const struct dir *d, const uint8_t *aad, const uint8_t *ct, size_t n, const uint8_t *tag, uint8_t *pt) {
    uint8_t nonce[12];
    make_nonce(d, nonce);
    if (t->suite == SUITE_CHACHA) return chacha20poly1305_open(d->key, nonce, aad, 5, ct, n, tag, pt);
    return aes128gcm_open(d->key, nonce, aad, 5, ct, n, tag, pt);
}

/* ---------- отправка записи ---------- */

/* ---- TLS 1.2: запись AEAD. nonce GCM = соль(4) + номер(8); ChaCha20-Poly1305 (RFC 7905) = iv XOR номер ---- */
static int is_chacha12(const struct tls *t) { return t->suite == 0xCCA8 || t->suite == 0xCCA9; }

static void aad12(uint8_t aad[13], uint64_t seq, uint8_t type, size_t n) {
    for (int i = 0; i < 8; i++) aad[i] = (uint8_t)(seq >> (56 - 8 * i));
    aad[8] = type; aad[9] = 3; aad[10] = 3; aad[11] = (uint8_t)(n >> 8); aad[12] = (uint8_t)n;
}

static int send_record(struct tls *t, uint8_t type, const void *data, size_t len) {
    uint8_t *w = t->wbuf;
    if (len > REC_MAX) return TLS_E_PROTO;
    if (t->v12 && t->tx.on) {
        uint8_t aad[13], tag[16], nonce[12];
        aad12(aad, t->tx.seq, type, len);
        size_t ex = is_chacha12(t) ? 0 : 8;
        w[0] = type; put16(w + 1, 0x0303); put16(w + 3, (unsigned)(ex + len + 16));
        if (ex) {
            memcpy(nonce, t->tx.iv, 4);
            for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(t->tx.seq >> (56 - 8 * i));
            memcpy(w + 5, nonce + 4, 8);
            memcpy(w + 5 + ex, data, len);
            aes128gcm_seal(t->tx.key, nonce, aad, 13, w + 5 + ex, len, w + 5 + ex, tag);
        } else {
            make_nonce(&t->tx, nonce);
            memcpy(w + 5, data, len);
            chacha20poly1305_seal(t->tx.key, nonce, aad, 13, w + 5, len, w + 5, tag);
        }
        memcpy(w + 5 + ex + len, tag, 16);
        t->tx.seq++;
        return t->io.send(t->io.ctx, w, 5 + ex + len + 16) == 0 ? TLS_OK : TLS_E_IO;
    }
    if (!t->tx.on) {
        w[0] = type; put16(w + 1, 0x0303); put16(w + 3, (unsigned)len);
        memcpy(w + 5, data, len);
        return t->io.send(t->io.ctx, w, 5 + len) == 0 ? TLS_OK : TLS_E_IO;
    }
    /* TLSInnerPlaintext = данные || тип; всё шифруется, снаружи тип 23 */
    size_t n = len + 1;
    w[0] = 23; put16(w + 1, 0x0303); put16(w + 3, (unsigned)(n + 16));
    memcpy(w + 5, data, len);
    w[5 + len] = type;
    uint8_t tag[16];
    aead_seal(t, &t->tx, w, w + 5, n, w + 5, tag);
    memcpy(w + 5 + n, tag, 16);
    t->tx.seq++;
    return t->io.send(t->io.ctx, w, 5 + n + 16) == 0 ? TLS_OK : TLS_E_IO;
}

static int send_hs(struct tls *t, const uint8_t *msg, size_t len) {
    sha256_update(&t->th, msg, len);
    return send_record(t, 22, msg, len);
}

/* ---------- приём записи ---------- */

/* 1 — запись целиком в rbuf, 0 — таймаут, <0 — ошибка */
static int rec_fill(struct tls *t, int timeout_ms) {
    for (;;) {
        size_t need = 5;
        if (t->rfill >= 5) {
            size_t rl = get16(t->rbuf + 3);
            if (rl > REC_MAX + 256) return TLS_E_PROTO;
            need = 5 + rl;
        }
        if (t->rfill >= need) return 1;
        int n = t->io.recv(t->io.ctx, t->rbuf + t->rfill, need - t->rfill, timeout_ms);
        if (n == 0) return 0;
        if (n < 0) return TLS_E_IO;
        t->rfill += (size_t)n;
    }
}

/* Достаёт следующую «настоящую» запись: расшифровывает, пропускает ChangeCipherSpec.
   Возвращает 1 и заполняет *type, *data, *len; 0 — таймаут; <0 — ошибка. */
static int rec_next(struct tls *t, int timeout_ms, int *type, uint8_t **data, size_t *len) {
    for (;;) {
        int r = rec_fill(t, timeout_ms);
        if (r <= 0) return r;
        uint8_t *b = t->rbuf;
        size_t rl = t->rfill - 5;
        int ot = b[0];
        t->rfill = 0;
        if (ot == 20) {                                  /* ChangeCipherSpec: в 1.3 игнорируем, в 1.2 включает шифрование приёма */
            if (t->v12 && t->rx.pend && !t->rx.on) { t->rx.on = 1; t->rx.pend = 0; t->rx.seq = 0; }
            continue;
        }
        if (!t->rx.on) { *type = ot; *data = b + 5; *len = rl; return 1; }
        if (t->v12) {
            size_t ex = is_chacha12(t) ? 0 : 8;
            if (rl < ex + 16) return TLS_E_PROTO;
            size_t n = rl - ex - 16;
            uint8_t aad[13], nonce[12];
            aad12(aad, t->rx.seq, (uint8_t)ot, n);
            uint8_t *ct = b + 5 + ex;
            int bad;
            if (ex) {
                memcpy(nonce, t->rx.iv, 4); memcpy(nonce + 4, b + 5, 8);
                bad = aes128gcm_open(t->rx.key, nonce, aad, 13, ct, n, ct + n, ct);
            } else {
                make_nonce(&t->rx, nonce);
                bad = chacha20poly1305_open(t->rx.key, nonce, aad, 13, ct, n, ct + n, ct);
            }
            if (bad) return TLS_E_CRYPTO;
            t->rx.seq++;
            *type = ot; *data = ct; *len = n;
            return 1;
        }
        if (ot != 23) return TLS_E_PROTO;                /* после ключей всё должно быть зашифровано */
        if (rl < 17) return TLS_E_PROTO;
        size_t n = rl - 16;
        if (aead_open(t, &t->rx, b, b + 5, n, b + 5 + n, b + 5) != 0) return TLS_E_CRYPTO;
        t->rx.seq++;
        while (n > 0 && b[5 + n - 1] == 0) n--;          /* снимаем выравнивание */
        if (n == 0) return TLS_E_PROTO;
        *type = b[5 + n - 1];
        *data = b + 5;
        *len = n - 1;
        return 1;
    }
}

static int handle_alert(struct tls *t, const uint8_t *d, size_t n) {
    if (n < 2) return TLS_E_PROTO;
    t->info.alert = d[1];
    if (d[1] == 0) return TLS_EOF;                       /* close_notify */
    return TLS_E_ALERT;
}

/* ---------- сообщения рукопожатия (с учётом дробления по записям) ---------- */

/* Получает следующее сообщение рукопожатия целиком. Указатель действует до следующего вызова. */
static int hs_next(struct tls *t, int *mtype, const uint8_t **body, size_t *blen, const uint8_t **whole, size_t *wlen) {
    if (t->hs_used) {                                     /* убираем прошлое сообщение */
        memmove(t->hs, t->hs + t->hs_used, t->hs_len - t->hs_used);
        t->hs_len -= t->hs_used;
        t->hs_used = 0;
    }
    for (;;) {
        if (t->hs_len >= 4) {
            size_t bl = get24(t->hs + 1);
            if (4 + bl > HS_CAP) return TLS_E_UNSUPPORTED;
            if (t->hs_len >= 4 + bl) {
                *mtype = t->hs[0]; *body = t->hs + 4; *blen = bl;
                *whole = t->hs; *wlen = 4 + bl;
                t->hs_used = 4 + bl;
                return TLS_OK;
            }
        }
        int type; uint8_t *d; size_t n;
        int r = rec_next(t, IO_WAIT, &type, &d, &n);
        if (r == 0) return TLS_E_TIMEOUT;
        if (r < 0) return r;
        if (type == 21) { int e = handle_alert(t, d, n); return e == TLS_EOF ? TLS_E_ALERT : e; }
        if (type != 22) return TLS_E_PROTO;
        if (t->hs_len + n > HS_CAP) return TLS_E_UNSUPPORTED;
        memcpy(t->hs + t->hs_len, d, n);
        t->hs_len += n;
    }
}

/* ---------- ClientHello ---------- */

static size_t build_client_hello(uint8_t *o, const char *host, const uint8_t random[32], const uint8_t *sid, size_t sidlen,
                                 const uint8_t pub[32], int flags) {
    int only12 = (flags & TLS_F_ONLY12) != 0;
    size_t n = 4;                                         /* место под заголовок рукопожатия */
    put16(o + n, 0x0303); n += 2;
    memcpy(o + n, random, 32); n += 32;
    o[n++] = (uint8_t)sidlen; memcpy(o + n, sid, sidlen); n += sidlen;
    size_t nsuites = only12 ? 4 : 6;
    put16(o + n, (unsigned)(nsuites * 2)); n += 2;
    if (!only12) { put16(o + n, SUITE_CHACHA); n += 2; put16(o + n, SUITE_AES128); n += 2; }
    put16(o + n, 0xCCA8); n += 2;                         /* ECDHE-RSA-CHACHA20-POLY1305 */
    put16(o + n, 0xCCA9); n += 2;                         /* ECDHE-ECDSA-CHACHA20-POLY1305 */
    put16(o + n, 0xC02F); n += 2;                         /* ECDHE-RSA-AES128-GCM-SHA256 */
    put16(o + n, 0xC02B); n += 2;                         /* ECDHE-ECDSA-AES128-GCM-SHA256 */
    o[n++] = 1; o[n++] = 0;                               /* без сжатия */
    size_t ext_len_pos = n; n += 2;
    size_t hl = strlen(host);
    if (hl > 200) hl = 200;
    /* server_name (для IP-адреса не шлём: по RFC 6066 там только имена) */
    int ip_lit = hl > 0;
    for (size_t i = 0; i < hl; i++) if (!((host[i] >= '0' && host[i] <= '9') || host[i] == '.')) { ip_lit = 0; break; }
    if (hl > 0 && !ip_lit) {
        put16(o + n, 0); n += 2; put16(o + n, (unsigned)(hl + 5)); n += 2;
        put16(o + n, (unsigned)(hl + 3)); n += 2; o[n++] = 0; put16(o + n, (unsigned)hl); n += 2;
        memcpy(o + n, host, hl); n += hl;
    }
    /* supported_groups: x25519, secp256r1 (P-256 нужен серверам с ECDSA-сертификатом и тем, кто выбирает её сам) */
    put16(o + n, 10); n += 2; put16(o + n, 6); n += 2; put16(o + n, 4); n += 2; put16(o + n, 0x001d); n += 2; put16(o + n, 0x0017); n += 2;
    /* ec_point_formats: uncompressed (для ECDHE в 1.2) */
    put16(o + n, 11); n += 2; put16(o + n, 2); n += 2; o[n++] = 1; o[n++] = 0;
    /* signature_algorithms */
    static const uint16_t sigs[] = { 0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0401, 0x0501, 0x0807 };
    put16(o + n, 13); n += 2; put16(o + n, 2 + 2 * 8); n += 2; put16(o + n, 2 * 8); n += 2;
    for (int i = 0; i < 8; i++) { put16(o + n, sigs[i]); n += 2; }
    /* extended_master_secret (RFC 7627) и renegotiation_info (RFC 5746, пустое) */
    put16(o + n, 23); n += 2; put16(o + n, 0); n += 2;
    put16(o + n, 0xff01); n += 2; put16(o + n, 1); n += 2; o[n++] = 0;
    if (!only12) {
        /* supported_versions: 1.3 и 1.2 */
        put16(o + n, 43); n += 2; put16(o + n, 5); n += 2; o[n++] = 4; put16(o + n, 0x0304); n += 2; put16(o + n, 0x0303); n += 2;
        /* key_share: x25519 */
        put16(o + n, 51); n += 2; put16(o + n, 38); n += 2; put16(o + n, 36); n += 2;
        put16(o + n, 0x001d); n += 2; put16(o + n, 32); n += 2; memcpy(o + n, pub, 32); n += 32;
    }
    put16(o + ext_len_pos, (unsigned)(n - ext_len_pos - 2));
    o[0] = 1; put24(o + 1, (unsigned)(n - 4));
    return n;
}

/* ---------- рукопожатие ---------- */

static const uint8_t HRR_RANDOM[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
    0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C };

static void zeroize(void *p, size_t n) { volatile uint8_t *v = p; while (n--) *v++ = 0; }

/* ---------- TLS 1.2 ---------- */

/* PRF из RFC 5246: P_SHA256(secret, label + seed1 + seed2) */
static void prf12(const uint8_t *secret, size_t sl, const char *label, const uint8_t *s1, size_t l1,
                  const uint8_t *s2, size_t l2, uint8_t *out, size_t olen) {
    uint8_t seed[160], a[32], buf[32 + 160], blk[32];
    size_t ll = strlen(label), n = 0;
    memcpy(seed, label, ll); n = ll;
    memcpy(seed + n, s1, l1); n += l1;
    if (l2) { memcpy(seed + n, s2, l2); n += l2; }
    hmac_sha256(secret, sl, seed, n, a);                          /* A(1) */
    while (olen) {
        memcpy(buf, a, 32); memcpy(buf + 32, seed, n);
        hmac_sha256(secret, sl, buf, 32 + n, blk);
        size_t k = olen < 32 ? olen : 32;
        memcpy(out, blk, k); out += k; olen -= k;
        hmac_sha256(secret, sl, a, 32, a);                        /* A(i+1) */
    }
    secure_zero(buf, sizeof buf);
}

static void keys12(struct tls *t) {
    uint8_t kb[88];
    int ch = is_chacha12(t);
    size_t kl = ch ? 32 : 16, il = ch ? 12 : 4;
    prf12(t->master, 48, "key expansion", t->sr, 32, t->cr, 32, kb, 2 * (kl + il));
    memset(&t->tx, 0, sizeof t->tx); memset(&t->rx, 0, sizeof t->rx);
    memcpy(t->tx.key, kb, kl);           memcpy(t->rx.key, kb + kl, kl);
    memcpy(t->tx.iv, kb + 2 * kl, il);   memcpy(t->rx.iv, kb + 2 * kl + il, il);
    t->rx.pend = 1;                      /* приём включится по ChangeCipherSpec сервера */
    secure_zero(kb, sizeof kb);
}

static int send_ccs(struct tls *t) {
    uint8_t rec[6] = { 20, 3, 3, 0, 1, 1 };
    if (t->io.send(t->io.ctx, rec, 6) != 0) return TLS_E_IO;
    t->tx.on = 1; t->tx.seq = 0;
    return TLS_OK;
}

static int send_finished12(struct tls *t) {
    uint8_t th[32], fin[16];
    th_snapshot(t, th);
    prf12(t->master, 48, "client finished", th, 32, NULL, 0, fin + 4, 12);
    fin[0] = 20; put24(fin + 1, 12);
    return send_hs(t, fin, 16);
}

/* ждёт Finished сервера (по пути пропуская NewSessionTicket), проверяет его */
static int recv_finished12(struct tls *t) {
    for (;;) {
        int mt; const uint8_t *body, *whole; size_t bl, wl;
        int r = hs_next(t, &mt, &body, &bl, &whole, &wl);
        if (r) return r;
        if (mt == 4) { sha256_update(&t->th, whole, wl); continue; }      /* NewSessionTicket: нас это не касается, но он в стенограмме */
        if (mt != 20 || bl != 12 || !t->rx.on) return TLS_E_PROTO;
        uint8_t th[32], want[12];
        th_snapshot(t, th);
        prf12(t->master, 48, "server finished", th, 32, NULL, 0, want, 12);
        if (ct_memcmp(want, body, 12) != 0) return TLS_E_CRYPTO;
        sha256_update(&t->th, whole, wl);
        return TLS_OK;
    }
}

/* ServerHello уже разобран и добавлен в стенограмму. 1.2: полное рукопожатие или возобновление. */
static int handshake12(struct tls *t, const uint8_t priv[32], const struct tls_session *resume, int resuming) {
    int r, mt; const uint8_t *body, *whole; size_t bl, wl;
    t->v12 = 1;
    t->info.version = 12;
    t->info.cipher = t->suite == 0xCCA8 || t->suite == 0xCCA9 ? "TLS1.2 ECDHE-CHACHA20-POLY1305" : "TLS1.2 ECDHE-AES128-GCM-SHA256";
    if (resuming) {
        memcpy(t->master, resume->master, 48);
        t->info.resumed = 1;
        keys12(t);
        if ((r = recv_finished12(t)) != 0) return r;
        if ((r = send_ccs(t)) != 0) return r;
        return send_finished12(t);
    }
    int cert_request = 0, state = 0;       /* 0 Certificate, 1 ServerKeyExchange, 2 до ServerHelloDone */
    uint8_t spub[65]; int have_spub = 0, curve = 0;       /* curve: 1 = x25519, 2 = P-256 */
    for (;;) {
        r = hs_next(t, &mt, &body, &bl, &whole, &wl);
        if (r) return r;
        if (mt == 11 && state == 0) {
            if (bl >= 6) {
                size_t listlen = get24(body);
                if (3 + listlen <= bl && listlen >= 3) {
                    size_t c0 = get24(body + 3);
                    if (3 + c0 <= listlen) { sha256(body + 6, c0, t->info.cert_sha256); t->info.have_cert = 1; }
                }
            }
            state = 1;
        } else if (mt == 12 && state <= 1) {
            /* ECDH: тип кривой (3 = named_curve), кривая, длина точки, точка; дальше подпись — пока не проверяем, как и в 1.3 */
            if (bl < 4 || body[0] != 3) return TLS_E_UNSUPPORTED;
            unsigned cv = get16(body + 1), pl = body[3];
            if (cv == 0x001d && pl == 32 && bl >= 4 + 32) { curve = 1; memcpy(spub, body + 4, 32); }
            else if (cv == 0x0017 && pl == 65 && bl >= 4 + 65) { curve = 2; memcpy(spub, body + 4, 65); }
            else return TLS_E_UNSUPPORTED;
            have_spub = 1;
            state = 2;
        } else if (mt == 13 && state == 2) {
            cert_request = 1;
        } else if (mt == 22) {
            /* CertificateStatus — игнорируем */
        } else if (mt == 14 && state == 2) {
            sha256_update(&t->th, whole, wl);
            break;
        } else return TLS_E_PROTO;
        sha256_update(&t->th, whole, wl);
    }
    if (!have_spub) return TLS_E_PROTO;
    uint8_t pms[32], cpub[65]; size_t cpl;
    if (curve == 1) {
        x25519_base(cpub, priv); cpl = 32;
        x25519(pms, priv, spub);
        uint8_t acc = 0; for (int i = 0; i < 32; i++) acc |= pms[i]; if (!acc) return TLS_E_CRYPTO;
    } else {
        uint8_t ppriv[32];
        if (p256_keygen(ppriv, cpub) != 0) return TLS_E_CRYPTO;
        cpl = 65;
        int bad = p256_ecdh(pms, ppriv, spub);
        zeroize(ppriv, 32);
        if (bad) return TLS_E_CRYPTO;                      /* точка сервера не лежит на кривой */
    }
    if (cert_request) {                                   /* сертификата у нас нет — отправляем пустой список */
        uint8_t m[7] = { 11, 0, 0, 3, 0, 0, 0 };
        if ((r = send_hs(t, m, 7)) != 0) return r;
    }
    uint8_t cke[4 + 1 + 65];
    cke[0] = 16; put24(cke + 1, (unsigned)(1 + cpl)); cke[4] = (uint8_t)cpl; memcpy(cke + 5, cpub, cpl);
    if ((r = send_hs(t, cke, 5 + cpl)) != 0) return r;
    if (t->ems) {
        uint8_t sh[32]; th_snapshot(t, sh);
        prf12(pms, 32, "extended master secret", sh, 32, NULL, 0, t->master, 48);
    } else {
        prf12(pms, 32, "master secret", t->cr, 32, t->sr, 32, t->master, 48);
    }
    secure_zero(pms, 32);
    keys12(t);
    if ((r = send_ccs(t)) != 0) return r;
    if ((r = send_finished12(t)) != 0) return r;
    return recv_finished12(t);
}

static int handshake(struct tls *t, const char *host, int flags, const struct tls_session *resume) {
    uint8_t priv[32], pub[32], random[32], sid[32], ch[640];
    int only12 = (flags & TLS_F_ONLY12) != 0;
    rng_bytes(priv, 32); rng_bytes(random, 32);
    x25519_base(pub, priv);
    size_t sidlen;
    int can_resume = resume && resume->valid && resume->idlen > 0;
    if (can_resume) { memcpy(sid, resume->id, resume->idlen); sidlen = resume->idlen; only12 = 1; flags |= TLS_F_ONLY12; }
    else if (only12) sidlen = 0;
    else { rng_bytes(sid, 32); sidlen = 32; }
    t->flags = flags;
    memcpy(t->cr, random, 32);
    size_t chl = build_client_hello(ch, host, random, sid, sidlen, pub, flags);
    sha256_init(&t->th);
    int r = send_hs(t, ch, chl);
    if (r) return r;

    /* --- ServerHello --- */
    int mt; const uint8_t *body, *whole; size_t bl, wl;
    r = hs_next(t, &mt, &body, &bl, &whole, &wl);
    if (r) return r;
    if (mt != 2) return TLS_E_PROTO;
    if (bl < 2 + 32 + 1) return TLS_E_PROTO;
    const uint8_t *p = body, *end = body + bl;
    p += 2;
    if (memcmp(p, HRR_RANDOM, 32) == 0) return TLS_E_UNSUPPORTED;
    memcpy(t->sr, p, 32);
    p += 32;
    size_t sl = *p++;
    if (sl > 32 || p + sl + 3 > end) return TLS_E_PROTO;
    const uint8_t *ssid = p; p += sl;
    unsigned suite = get16(p); p += 2;
    if (*p++ != 0) return TLS_E_PROTO;
    int got_ver = 0, got_ems = 0; const uint8_t *spub = NULL;
    if (p + 2 <= end) {
        size_t el = get16(p); p += 2;
        if (p + el > end) return TLS_E_PROTO;
        end = p + el;
        while (p + 4 <= end) {
            unsigned et = get16(p), elen = get16(p + 2); p += 4;
            if (p + elen > end) return TLS_E_PROTO;
            if (et == 43 && elen == 2) { if (get16(p) != 0x0304) return TLS_E_UNSUPPORTED; got_ver = 1; }
            if (et == 51 && elen == 36) { if (get16(p) != 0x001d || get16(p + 2) != 32) return TLS_E_UNSUPPORTED; spub = p + 4; }
            if (et == 23) got_ems = 1;
            p += elen;
        }
    }
    if (!got_ver) {
        /* сервер выбрал TLS 1.2 */
        if (suite != 0xCCA8 && suite != 0xCCA9 && suite != 0xC02F && suite != 0xC02B) return TLS_E_UNSUPPORTED;
        if (!only12 && memcmp(t->sr + 24, "DOWNGRD\x01", 8) == 0) return TLS_E_CRYPTO;   /* защита от понижения версии */
        t->suite = (int)suite;
        t->ems = got_ems;
        sha256_update(&t->th, whole, wl);
        if (t->hs_len != t->hs_used) return TLS_E_PROTO;
        t->hs_len = t->hs_used = 0;
        int resuming = can_resume && sl == resume->idlen && memcmp(ssid, resume->id, sl) == 0;
        if (resuming && (suite != resume->suite || got_ems != resume->ems)) return TLS_E_PROTO;
        memcpy(t->sid, ssid, sl); t->sidlen = (uint8_t)sl;
        r = handshake12(t, priv, resume, resuming);
        zeroize(priv, 32);
        return r;
    }
    if (only12) return TLS_E_PROTO;
    if (!spub) return TLS_E_UNSUPPORTED;
    if (sl != 32 || memcmp(ssid, sid, 32) != 0) return TLS_E_PROTO;
    if (suite != SUITE_CHACHA && suite != SUITE_AES128) return TLS_E_UNSUPPORTED;
    t->info.version = 13;
    t->suite = (int)suite;
    t->keylen = suite == SUITE_CHACHA ? 32 : 16;
    t->info.cipher = suite == SUITE_CHACHA ? "TLS_CHACHA20_POLY1305_SHA256" : "TLS_AES_128_GCM_SHA256";
    sha256_update(&t->th, whole, wl);
    if (t->hs_len != t->hs_used) return TLS_E_PROTO;     /* после ServerHello в той же записи ничего быть не должно */

    /* --- ключи рукопожатия --- */
    uint8_t shared[32], zeros[32] = { 0 }, empty_hash[32], early[32], derived[32], hs_secret[32], thash[32];
    uint8_t c_hs[32], s_hs[32];
    x25519(shared, priv, spub);
    zeroize(priv, 32);
    {   /* защита от точек малого порядка: общий секрет не должен быть нулевым */
        uint8_t acc = 0; for (int i = 0; i < 32; i++) acc |= shared[i];
        if (!acc) return TLS_E_CRYPTO;
    }
    sha256(NULL, 0, empty_hash);
    hkdf_extract(NULL, 0, zeros, 32, early);
    tls13_expand_label(early, "derived", empty_hash, 32, derived, 32);
    hkdf_extract(derived, 32, shared, 32, hs_secret);
    zeroize(shared, 32);
    th_snapshot(t, thash);
    tls13_expand_label(hs_secret, "c hs traffic", thash, 32, c_hs, 32);
    tls13_expand_label(hs_secret, "s hs traffic", thash, 32, s_hs, 32);
    dir_set_keys(t, &t->rx, s_hs);                        /* с этого момента сервер шифрует */
    t->hs_len = t->hs_used = 0;

    /* --- EncryptedExtensions, Certificate, CertificateVerify, Finished --- */
    int cert_request = 0, state = 0;                      /* 0 жду EE, 1 Cert, 2 CV, 3 Fin */
    for (;;) {
        r = hs_next(t, &mt, &body, &bl, &whole, &wl);
        if (r) return r;
        if (state == 0) {
            if (mt != 8) return TLS_E_PROTO;
            state = 1;
        } else if (state == 1) {
            if (mt == 13) { cert_request = 1; }           /* сервер просит сертификат клиента — отвечаем пустым */
            else if (mt == 11) {
                /* context(1) + список: 3 байта длины, затем сертификаты по (3 длина, данные, 2 длина расширений, расширения) */
                if (bl < 1) return TLS_E_PROTO;
                size_t cl = body[0];
                if (bl < 1 + cl + 3) return TLS_E_PROTO;
                const uint8_t *q = body + 1 + cl;
                size_t listlen = get24(q); q += 3;
                if (q + listlen > body + bl) return TLS_E_PROTO;
                if (listlen >= 3) {
                    size_t c0 = get24(q);
                    if (3 + c0 <= listlen) {
                        sha256(q + 3, c0, t->info.cert_sha256);
                        t->info.have_cert = 1;
                    }
                }
                state = 2;
            } else return TLS_E_PROTO;
        } else if (state == 2) {
            if (mt != 15) return TLS_E_PROTO;             /* CertificateVerify — подпись пока не проверяем */
            state = 3;
        } else {
            if (mt != 20 || bl != 32) return TLS_E_PROTO;
            uint8_t fk[32], want[32];
            th_snapshot(t, thash);                        /* стенограмма до Finished */
            tls13_expand_label(s_hs, "finished", NULL, 0, fk, 32);
            hmac_sha256(fk, 32, thash, 32, want);
            if (ct_memcmp(want, body, 32) != 0) return TLS_E_CRYPTO;
            sha256_update(&t->th, whole, wl);
            break;
        }
        sha256_update(&t->th, whole, wl);
    }

    /* --- ключи приложения (стенограмма по server Finished) --- */
    uint8_t derived2[32], master[32], c_ap[32], s_ap[32];
    th_snapshot(t, thash);
    tls13_expand_label(hs_secret, "derived", empty_hash, 32, derived2, 32);
    hkdf_extract(derived2, 32, zeros, 32, master);
    tls13_expand_label(master, "c ap traffic", thash, 32, c_ap, 32);
    tls13_expand_label(master, "s ap traffic", thash, 32, s_ap, 32);

    /* --- наш ответ: [пустой Certificate] + Finished --- */
    uint8_t ccs = 1;
    r = TLS_OK;
    { uint8_t hdr[5] = { 20, 3, 3, 0, 1 }, rec[6]; memcpy(rec, hdr, 5); rec[5] = ccs;
      if (t->io.send(t->io.ctx, rec, 6) != 0) return TLS_E_IO; }
    dir_set_keys(t, &t->tx, c_hs);
    if (cert_request) {
        uint8_t m[8] = { 11, 0, 0, 4, 0, 0, 0, 0 };
        r = send_hs(t, m, 8);
        if (r) return r;
    }
    {
        uint8_t fk[32], fin[4 + 32], th2[32];
        th_snapshot(t, th2);                              /* стенограмма по server Finished (и наш Certificate, если был) */
        tls13_expand_label(c_hs, "finished", NULL, 0, fk, 32);
        fin[0] = 20; put24(fin + 1, 32);
        hmac_sha256(fk, 32, th2, 32, fin + 4);
        r = send_hs(t, fin, sizeof fin);
        if (r) return r;
    }
    dir_set_keys(t, &t->tx, c_ap);
    dir_set_keys(t, &t->rx, s_ap);
    zeroize(c_hs, 32); zeroize(s_hs, 32); zeroize(hs_secret, 32); zeroize(master, 32);
    return TLS_OK;
}

struct tls *tls_open_ex(const struct tls_io *io, const char *host, int flags, const struct tls_session *resume, int *err) {
    int dummy; if (!err) err = &dummy;
    *err = TLS_E_MEM;
    struct tls *t = kmalloc(sizeof *t);
    if (!t) return NULL;
    memset(t, 0, sizeof *t);
    t->io = *io;
    t->rbuf = kmalloc(REC_BUF); t->wbuf = kmalloc(REC_BUF); t->hs = kmalloc(HS_CAP);
    if (!t->rbuf || !t->wbuf || !t->hs) goto fail;
    int r = handshake(t, host, flags, resume);
    kfree(t->hs); t->hs = NULL;
    if (r != TLS_OK) {
        /* при ошибке по нашей вине вежливо сообщаем серверу */
        if (r == TLS_E_CRYPTO || r == TLS_E_PROTO || r == TLS_E_UNSUPPORTED) {
            uint8_t al[2] = { 2, (uint8_t)(r == TLS_E_CRYPTO ? 51 : (r == TLS_E_UNSUPPORTED ? 40 : 10)) };
            send_record(t, 21, al, 2);
        }
        *err = r;
        goto fail;
    }
    *err = TLS_OK;
    return t;
fail:
    if (t->rbuf) kfree(t->rbuf);
    if (t->wbuf) kfree(t->wbuf);
    if (t->hs) kfree(t->hs);
    kfree(t);
    return NULL;
}

struct tls *tls_open(const struct tls_io *io, const char *host, int *err) {
    return tls_open_ex(io, host, 0, NULL, err);
}

int tls_session_save(const struct tls *t, struct tls_session *out) {
    memset(out, 0, sizeof *out);
    if (!t || !t->v12 || t->sidlen == 0) return -1;
    out->valid = 1; out->idlen = t->sidlen; memcpy(out->id, t->sid, t->sidlen);
    memcpy(out->master, t->master, 48);
    out->suite = (uint16_t)t->suite; out->ems = t->ems;
    return 0;
}

int tls_write(struct tls *t, const void *data, size_t len) {
    const uint8_t *d = data;
    if (t->closed) return t->err ? t->err : TLS_EOF;
    while (len) {
        size_t k = len > 16000 ? 16000 : len;
        int r = send_record(t, 23, d, k);
        if (r) { t->err = r; return r; }
        d += k; len -= k;
    }
    return TLS_OK;
}

static int key_update(struct tls *t, int reply) {
    uint8_t ns[32];
    if (reply) {
        uint8_t m[5] = { 24, 0, 0, 1, 0 };
        int r = send_record(t, 22, m, 5);                 /* KeyUpdate(update_not_requested) на старых ключах */
        if (r) return r;
        tls13_expand_label(t->tx.secret, "traffic upd", NULL, 0, ns, 32);
        dir_set_keys(t, &t->tx, ns);
    }
    tls13_expand_label(t->rx.secret, "traffic upd", NULL, 0, ns, 32);
    dir_set_keys(t, &t->rx, ns);
    return TLS_OK;
}

int tls_read(struct tls *t, void *buf, size_t max, int timeout_ms) {
    if (max == 0) return 0;
    for (;;) {
        if (t->pt_valid && t->pt_len > 0) {
            size_t k = t->pt_len < max ? t->pt_len : max;
            memcpy(buf, t->rbuf + 5 + t->pt_off, k);
            t->pt_off += k; t->pt_len -= k;
            return (int)k;
        }
        t->pt_valid = 0;
        if (t->closed) return t->err ? t->err : TLS_EOF;
        int type; uint8_t *d; size_t n;
        int r = rec_next(t, timeout_ms, &type, &d, &n);
        if (r == 0) return 0;
        if (r < 0) { t->closed = 1; t->err = r; return r == TLS_E_IO ? TLS_EOF : r; }
        if (type == 23) {
            t->pt_off = (size_t)(d - (t->rbuf + 5)); t->pt_len = n; t->pt_valid = 1;
            continue;
        }
        if (type == 21) { r = handle_alert(t, d, n); t->closed = 1; t->err = r; return r; }
        if (type == 22) {
            /* после рукопожатия: билеты сессий игнорируем, KeyUpdate выполняем */
            size_t off = 0;
            while (off + 4 <= n) {
                size_t ml = get24(d + off + 1);
                if (off + 4 + ml > n) break;
                if (!t->v12 && d[off] == 24 && ml == 1) {
                    int e = key_update(t, d[off + 4] == 1);
                    if (e) { t->closed = 1; t->err = e; return e; }
                }
                off += 4 + ml;
            }
            continue;
        }
        t->closed = 1; t->err = TLS_E_PROTO;
        return TLS_E_PROTO;
    }
}

void tls_close(struct tls *t) {
    if (!t) return;
    if (!t->closed && t->tx.on) { uint8_t al[2] = { 1, 0 }; send_record(t, 21, al, 2); }
    zeroize(&t->rx, sizeof t->rx); zeroize(&t->tx, sizeof t->tx); zeroize(t->master, 48);
    kfree(t->rbuf); kfree(t->wbuf);
    if (t->hs) kfree(t->hs);
    kfree(t);
}
