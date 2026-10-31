#include "crypto.h"
#include "kstring.h"

/* ====================== утилиты ====================== */

static inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static inline uint32_t ld32le(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline void st32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline uint32_t ld32be(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline void st32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline uint64_t ld64be(const uint8_t *p) { return ((uint64_t)ld32be(p) << 32) | ld32be(p + 4); }
static inline void st64be(uint8_t *p, uint64_t v) { st32be(p, (uint32_t)(v >> 32)); st32be(p + 4, (uint32_t)v); }

int ct_memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d;
}

/* ====================== SHA-256 ====================== */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_block(struct sha256 *c, const uint8_t *p) {
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = ld32be(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha256_init(struct sha256 *c) {
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    for (int i = 0; i < 8; i++) c->h[i] = iv[i];
    c->len = 0; c->n = 0;
}

void sha256_update(struct sha256 *c, const void *data, size_t len) {
    const uint8_t *p = data;
    c->len += len;
    while (len) {
        size_t k = 64 - c->n;
        if (k > len) k = len;
        memcpy(c->buf + c->n, p, k);
        c->n += k; p += k; len -= k;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}

void sha256_final(struct sha256 *c, uint8_t out[32]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) sha256_update(c, &pad, 1);
    uint8_t lb[8];
    st64be(lb, bits);
    sha256_update(c, lb, 8);
    for (int i = 0; i < 8; i++) st32be(out + 4 * i, c->h[i]);
}

void sha256(const void *data, size_t len, uint8_t out[32]) {
    struct sha256 c;
    sha256_init(&c); sha256_update(&c, data, len); sha256_final(&c, out);
}

/* ====================== HMAC / HKDF ====================== */

void hmac_sha256(const uint8_t *key, size_t klen, const void *data, size_t len, uint8_t out[32]) {
    uint8_t k[64], ipad[64], opad[64], inner[32];
    memset(k, 0, 64);
    if (klen > 64) sha256(key, klen, k); else if (klen) memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    struct sha256 c;
    sha256_init(&c); sha256_update(&c, ipad, 64); sha256_update(&c, data, len); sha256_final(&c, inner);
    sha256_init(&c); sha256_update(&c, opad, 64); sha256_update(&c, inner, 32); sha256_final(&c, out);
}

void hkdf_extract(const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen, uint8_t prk[32]) {
    static const uint8_t zeros[32] = { 0 };
    if (!salt || !slen) { salt = zeros; slen = 32; }
    hmac_sha256(salt, slen, ikm, ilen, prk);
}

void hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t ilen, uint8_t *out, size_t olen) {
    uint8_t t[32], buf[32 + 255 + 1];
    size_t tl = 0, done = 0;
    uint8_t ctr = 1;
    while (done < olen) {
        size_t bl = 0;
        if (tl) { memcpy(buf, t, tl); bl = tl; }
        if (ilen > 255) ilen = 255;
        if (ilen) memcpy(buf + bl, info, ilen);
        bl += ilen;
        buf[bl++] = ctr++;
        hmac_sha256(prk, 32, buf, bl, t);
        tl = 32;
        size_t k = olen - done; if (k > 32) k = 32;
        memcpy(out + done, t, k);
        done += k;
    }
}

void tls13_expand_label(const uint8_t secret[32], const char *label,
                        const uint8_t *ctx, size_t clen, uint8_t *out, size_t olen) {
    uint8_t info[2 + 1 + 6 + 32 + 1 + 64];
    size_t ll = strlen(label), n = 0;
    info[n++] = (uint8_t)(olen >> 8); info[n++] = (uint8_t)olen;
    info[n++] = (uint8_t)(6 + ll);
    memcpy(info + n, "tls13 ", 6); n += 6;
    memcpy(info + n, label, ll); n += ll;
    info[n++] = (uint8_t)clen;
    if (clen) { memcpy(info + n, ctx, clen); n += clen; }
    hkdf_expand(secret, info, n, out, olen);
}

/* ====================== ChaCha20 ====================== */

#define QR(a, b, c, d) \
    a += b; d ^= a; d = rotl32(d, 16); \
    c += d; b ^= c; b = rotl32(b, 12); \
    a += b; d ^= a; d = rotl32(d, 8);  \
    c += d; b ^= c; b = rotl32(b, 7)

static void chacha_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = ld32le(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = ld32le(nonce + 4 * i);
    for (int i = 0; i < 16; i++) x[i] = s[i];
    for (int r = 0; r < 10; r++) {
        QR(x[0], x[4], x[8],  x[12]); QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]); QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) st32le(out + 4 * i, x[i] + s[i]);
}

static void chacha_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                       const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t ks[64];
    while (len) {
        chacha_block(key, counter++, nonce, ks);
        size_t k = len > 64 ? 64 : len;
        for (size_t i = 0; i < k; i++) out[i] = in[i] ^ ks[i];
        in += k; out += k; len -= k;
    }
}

/* ====================== Poly1305 (26-битные куски) ====================== */

struct poly { uint32_t r[5], h[5], pad[4]; uint8_t buf[16]; size_t n; };

static void poly_init(struct poly *p, const uint8_t key[32]) {
    p->r[0] = ld32le(key + 0) & 0x3ffffff;
    p->r[1] = (ld32le(key + 3) >> 2) & 0x3ffff03;
    p->r[2] = (ld32le(key + 6) >> 4) & 0x3ffc0ff;
    p->r[3] = (ld32le(key + 9) >> 6) & 0x3f03fff;
    p->r[4] = (ld32le(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 5; i++) p->h[i] = 0;
    for (int i = 0; i < 4; i++) p->pad[i] = ld32le(key + 16 + 4 * i);
    p->n = 0;
}

static void poly_block(struct poly *p, const uint8_t *m, uint32_t hibit) {
    uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2], r3 = p->r[3], r4 = p->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4];
    h0 += ld32le(m + 0) & 0x3ffffff;
    h1 += (ld32le(m + 3) >> 2) & 0x3ffffff;
    h2 += (ld32le(m + 6) >> 4) & 0x3ffffff;
    h3 += (ld32le(m + 9) >> 6) & 0x3ffffff;
    h4 += (ld32le(m + 12) >> 8) | hibit;
    uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
    uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
    uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
    uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
    uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
    uint32_t c;
    c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
    d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
    d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
    d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
    d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;
    p->h[0] = h0; p->h[1] = h1; p->h[2] = h2; p->h[3] = h3; p->h[4] = h4;
}

static void poly_update(struct poly *p, const uint8_t *m, size_t len) {
    while (len) {
        size_t k = 16 - p->n; if (k > len) k = len;
        memcpy(p->buf + p->n, m, k);
        p->n += k; m += k; len -= k;
        if (p->n == 16) { poly_block(p, p->buf, 1u << 24); p->n = 0; }
    }
}

static void poly_final(struct poly *p, uint8_t tag[16]) {
    if (p->n) {
        p->buf[p->n] = 1;
        for (size_t i = p->n + 1; i < 16; i++) p->buf[i] = 0;
        poly_block(p, p->buf, 0);
    }
    uint32_t h0 = p->h[0], h1 = p->h[1], h2 = p->h[2], h3 = p->h[3], h4 = p->h[4], c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;            /* 0xffffffff если h >= p */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    uint32_t t0 = h0 | (h1 << 26);
    uint32_t t1 = (h1 >> 6) | (h2 << 20);
    uint32_t t2 = (h2 >> 12) | (h3 << 14);
    uint32_t t3 = (h3 >> 18) | (h4 << 8);
    uint64_t f;
    f = (uint64_t)t0 + p->pad[0]; st32le(tag, (uint32_t)f);
    f = (uint64_t)t1 + p->pad[1] + (f >> 32); st32le(tag + 4, (uint32_t)f);
    f = (uint64_t)t2 + p->pad[2] + (f >> 32); st32le(tag + 8, (uint32_t)f);
    f = (uint64_t)t3 + p->pad[3] + (f >> 32); st32le(tag + 12, (uint32_t)f);
}

static void poly_pad16(struct poly *p, size_t len) {
    static const uint8_t z[16] = { 0 };
    if (len % 16) poly_update(p, z, 16 - len % 16);
}

static void chapoly_tag(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aadlen,
                        const uint8_t *ct, size_t len, uint8_t tag[16]) {
    uint8_t blk[64], lens[16];
    chacha_block(key, 0, nonce, blk);
    struct poly p;
    poly_init(&p, blk);
    poly_update(&p, aad, aadlen); poly_pad16(&p, aadlen);
    poly_update(&p, ct, len);     poly_pad16(&p, len);
    for (int i = 0; i < 8; i++) { lens[i] = (uint8_t)((uint64_t)aadlen >> (8 * i)); lens[8 + i] = (uint8_t)((uint64_t)len >> (8 * i)); }
    poly_update(&p, lens, 16);
    poly_final(&p, tag);
}

void chacha20poly1305_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aadlen,
                           const uint8_t *pt, size_t len, uint8_t *ct, uint8_t tag[16]) {
    chacha_xor(key, 1, nonce, pt, ct, len);
    chapoly_tag(key, nonce, aad, aadlen, ct, len, tag);
}

int chacha20poly1305_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aadlen,
                          const uint8_t *ct, size_t len, const uint8_t tag[16], uint8_t *pt) {
    uint8_t t[16];
    chapoly_tag(key, nonce, aad, aadlen, ct, len, t);
    if (ct_memcmp(t, tag, 16)) return -1;
    chacha_xor(key, 1, nonce, ct, pt, len);
    return 0;
}

/* ====================== AES-128-GCM ====================== */

static uint8_t sbox[256];
static int sbox_ready;

static void sbox_make(void) {
    uint8_t p = 1, q = 1;
    do {
        p = (uint8_t)(p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1B : 0));
        q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        uint8_t x = (uint8_t)(q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)));
        sbox[p] = (uint8_t)(x ^ 0x63);
    } while (p != 1);
    sbox[0] = 0x63;
    sbox_ready = 1;
}

struct aes128 { uint8_t rk[176]; };

static void aes128_key(struct aes128 *a, const uint8_t key[16]) {
    if (!sbox_ready) sbox_make();
    memcpy(a->rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, a->rk + i - 4, 4);
        if (i % 16 == 0) {
            uint8_t k = t[0];
            t[0] = (uint8_t)(sbox[t[1]] ^ rcon); t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[k];
            rcon = (uint8_t)((rcon << 1) ^ ((rcon & 0x80) ? 0x1B : 0));
        }
        for (int j = 0; j < 4; j++) a->rk[i + j] = (uint8_t)(a->rk[i - 16 + j] ^ t[j]);
    }
}

static inline uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1B)); }

static void aes128_enc(const struct aes128 *a, const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ a->rk[i];
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < 16; i++) t[i] = sbox[s[i]];
        /* ShiftRows */
        s[0] = t[0];  s[4] = t[4];  s[8] = t[8];   s[12] = t[12];
        s[1] = t[5];  s[5] = t[9];  s[9] = t[13];  s[13] = t[1];
        s[2] = t[10]; s[6] = t[14]; s[10] = t[2];  s[14] = t[6];
        s[3] = t[15]; s[7] = t[3];  s[11] = t[7];  s[15] = t[11];
        if (r != 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = s + 4 * c;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                uint8_t all = a0 ^ a1 ^ a2 ^ a3;
                col[0] = (uint8_t)(a0 ^ all ^ xtime(a0 ^ a1));
                col[1] = (uint8_t)(a1 ^ all ^ xtime(a1 ^ a2));
                col[2] = (uint8_t)(a2 ^ all ^ xtime(a2 ^ a3));
                col[3] = (uint8_t)(a3 ^ all ^ xtime(a3 ^ a0));
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= a->rk[16 * r + i];
    }
    memcpy(out, s, 16);
}

/* умножение в GF(2^128) для GHASH: x = x * h */
static void gf128_mul(uint64_t x[2], const uint64_t h[2]) {
    uint64_t zh = 0, zl = 0, vh = h[0], vl = h[1];
    for (int i = 0; i < 128; i++) {
        uint64_t bit = (i < 64) ? (x[0] >> (63 - i)) & 1 : (x[1] >> (127 - i)) & 1;
        uint64_t m = (uint64_t)0 - bit;
        zh ^= vh & m; zl ^= vl & m;
        uint64_t lsb = vl & 1;
        vl = (vl >> 1) | (vh << 63);
        vh = (vh >> 1) ^ (((uint64_t)0 - lsb) & 0xe100000000000000ULL);
    }
    x[0] = zh; x[1] = zl;
}

static void ghash_feed(uint64_t y[2], const uint64_t h[2], const uint8_t *d, size_t len) {
    while (len) {
        uint8_t b[16] = { 0 };
        size_t k = len > 16 ? 16 : len;
        memcpy(b, d, k);
        y[0] ^= ld64be(b); y[1] ^= ld64be(b + 8);
        gf128_mul(y, h);
        d += k; len -= k;
    }
}

static void gcm_ctr(const struct aes128 *a, const uint8_t nonce[12], const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t cb[16], ks[16];
    memcpy(cb, nonce, 12);
    uint32_t ctr = 2;
    while (len) {
        st32be(cb + 12, ctr++);
        aes128_enc(a, cb, ks);
        size_t k = len > 16 ? 16 : len;
        for (size_t i = 0; i < k; i++) out[i] = in[i] ^ ks[i];
        in += k; out += k; len -= k;
    }
}

static void gcm_tag(const struct aes128 *a, const uint8_t nonce[12], const uint8_t *aad, size_t aadlen,
                    const uint8_t *ct, size_t len, uint8_t tag[16]) {
    uint8_t zero[16] = { 0 }, hb[16], j0[16], ek[16], lb[16];
    aes128_enc(a, zero, hb);
    uint64_t h[2] = { ld64be(hb), ld64be(hb + 8) }, y[2] = { 0, 0 };
    ghash_feed(y, h, aad, aadlen);
    ghash_feed(y, h, ct, len);
    st64be(lb, (uint64_t)aadlen * 8); st64be(lb + 8, (uint64_t)len * 8);
    ghash_feed(y, h, lb, 16);
    memcpy(j0, nonce, 12); st32be(j0 + 12, 1);
    aes128_enc(a, j0, ek);
    st64be(tag, y[0]); st64be(tag + 8, y[1]);
    for (int i = 0; i < 16; i++) tag[i] ^= ek[i];
}

void aes128gcm_seal(const uint8_t key[16], const uint8_t nonce[12], const uint8_t *aad, size_t aadlen,
                    const uint8_t *pt, size_t len, uint8_t *ct, uint8_t tag[16]) {
    struct aes128 a;
    aes128_key(&a, key);
    gcm_ctr(&a, nonce, pt, ct, len);
    gcm_tag(&a, nonce, aad, aadlen, ct, len, tag);
}

int aes128gcm_open(const uint8_t key[16], const uint8_t nonce[12], const uint8_t *aad, size_t aadlen,
                   const uint8_t *ct, size_t len, const uint8_t tag[16], uint8_t *pt) {
    struct aes128 a;
    uint8_t t[16];
    aes128_key(&a, key);
    gcm_tag(&a, nonce, aad, aadlen, ct, len, t);
    if (ct_memcmp(t, tag, 16)) return -1;
    gcm_ctr(&a, nonce, ct, pt, len);
    return 0;
}

/* ====================== X25519 ====================== */

typedef int64_t gf[16];
static const gf gf_121665 = { 0xDB41, 1 };

static void car25519(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536;
    }
}

static void sel25519(gf p, gf q, int b) {
    int64_t c = ~((int64_t)b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t; q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2 * i] = (uint8_t)(t[i] & 0xff); o[2 * i + 1] = (uint8_t)(t[i] >> 8); }
}

static void unpack25519(gf o, const uint8_t *n) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void fadd(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fsub(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void fmul(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}
static void fsq(gf o, const gf a) { fmul(o, a, a); }

static void finv(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) {
        fsq(c, c);
        if (a != 2 && a != 4) fmul(c, c, i);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t z[32];
    gf x, a, b, c, d, e, f;
    for (int i = 0; i < 32; i++) z[i] = scalar[i];
    z[31] = (uint8_t)((scalar[31] & 127) | 64);
    z[0] &= 248;
    unpack25519(x, point);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; a[i] = c[i] = d[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; i--) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r); sel25519(c, d, r);
        fadd(e, a, c); fsub(a, a, c); fadd(c, b, d); fsub(b, b, d);
        fsq(d, e); fsq(f, a); fmul(a, c, a); fmul(c, b, e);
        fadd(e, a, c); fsub(a, a, c); fsq(b, a); fsub(c, d, f);
        fmul(a, c, gf_121665); fadd(a, a, d); fmul(c, c, a);
        fmul(a, d, f); fmul(d, b, x); fsq(b, e);
        sel25519(a, b, r); sel25519(c, d, r);
    }
    finv(c, c);
    fmul(a, a, c);
    pack25519(out, a);
}

void x25519_base(uint8_t out[32], const uint8_t scalar[32]) {
    uint8_t base[32] = { 9 };
    x25519(out, scalar, base);
}


/* ====================== SHA-512 (нужен для Ed25519, SSH) ====================== */

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
    0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
    0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL,
    0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
    0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL,
    0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
    0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL,
    0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL,
    0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
    0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
    0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};
static const uint64_t H512[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
};

static inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static void sha512_block(struct sha512 *c, const uint8_t *p) {
    uint64_t w[80], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = ld64be(p + 8 * i);
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + K512[i] + w[i];
        uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        uint64_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint64_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha512_init(struct sha512 *c) {
    for (int i = 0; i < 8; i++) c->h[i] = H512[i];
    c->len = 0; c->n = 0;
}

void sha512_update(struct sha512 *c, const void *data, size_t len) {
    const uint8_t *p = data;
    c->len += len;
    while (len) {
        size_t k = 128 - c->n; if (k > len) k = len;
        memcpy(c->buf + c->n, p, k);
        c->n += k; p += k; len -= k;
        if (c->n == 128) { sha512_block(c, c->buf); c->n = 0; }
    }
}

void sha512_final(struct sha512 *c, uint8_t out[64]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha512_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->n != 112) sha512_update(c, &z, 1);
    uint8_t lenb[16] = { 0 };
    st64be(lenb + 8, bits);                       /* длина < 2^61 байт — старшие 64 бита нулевые */
    sha512_update(c, lenb, 16);
    for (int i = 0; i < 8; i++) st64be(out + 8 * i, c->h[i]);
}

void sha512(const void *data, size_t len, uint8_t out[64]) {
    struct sha512 c;
    sha512_init(&c); sha512_update(&c, data, len); sha512_final(&c, out);
}

/* ====================== Ed25519: проверка подписи (RFC 8032) ======================
   Арифметика поля — та же, что у X25519 выше (представление TweetNaCl, 16 кусков по 16 бит).
   Подписывать нам не нужно: SSH-клиент только проверяет подпись хоста. */

static const gf gf0_, gf1_ = { 1 };
static const gf ED_D  = {0x78a3,0x1359,0x4dca,0x75eb,0xd8ab,0x4141,0x0a4d,0x0070,0xe898,0x7779,0x4079,0x8cc7,0xfe73,0x2b6f,0x6cee,0x5203};
static const gf ED_D2 = {0xf159,0x26b2,0x9b94,0xebd6,0xb156,0x8283,0x149a,0x00e0,0xd130,0xeef3,0x80f2,0x198e,0xfce7,0x56df,0xd9dc,0x2406};
static const gf ED_X  = {0xd51a,0x8f25,0x2d60,0xc956,0xa7b2,0x9525,0xc760,0x692c,0xdc5c,0xfdd6,0xe231,0xc0a4,0x53fe,0xcd6e,0x36d3,0x2169};
static const gf ED_Y  = {0x6658,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666};
static const gf ED_I  = {0xa0b0,0x4a0e,0x1b27,0xc4ee,0xe478,0xad2f,0x1806,0x2f43,0xd7a7,0x3dfb,0x0099,0x2b4d,0xdf0b,0x4fc1,0x2480,0x2b83};

static void gf_set(gf r, const gf a) { for (int i = 0; i < 16; i++) r[i] = a[i]; }
static int gf_neq(const gf a, const gf b) {
    uint8_t c[32], d[32];
    pack25519(c, a); pack25519(d, b);
    return ct_memcmp(c, d, 32) != 0;
}
static uint8_t gf_par(const gf a) { uint8_t d[32]; pack25519(d, a); return d[0] & 1; }

static void gf_pow2523(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 250; a >= 0; a--) { fsq(c, c); if (a != 1) fmul(c, c, i); }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

static void ed_add(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    fsub(a, p[1], p[0]); fsub(t, q[1], q[0]); fmul(a, a, t);
    fadd(b, p[0], p[1]); fadd(t, q[0], q[1]); fmul(b, b, t);
    fmul(c, p[3], q[3]); fmul(c, c, ED_D2);
    fmul(d, p[2], q[2]); fadd(d, d, d);
    fsub(e, b, a); fsub(f, d, c); fadd(g, d, c); fadd(h, b, a);
    fmul(p[0], e, f); fmul(p[1], h, g); fmul(p[2], g, f); fmul(p[3], e, h);
}

static void ed_cswap(gf p[4], gf q[4], int b) { for (int i = 0; i < 4; i++) sel25519(p[i], q[i], b); }

static void ed_pack(uint8_t *r, gf p[4]) {
    gf tx, ty, zi;
    finv(zi, p[2]); fmul(tx, p[0], zi); fmul(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= (uint8_t)(gf_par(tx) << 7);
}

static void ed_scalarmult(gf p[4], gf q[4], const uint8_t *s) {
    gf_set(p[0], gf0_); gf_set(p[1], gf1_); gf_set(p[2], gf1_); gf_set(p[3], gf0_);
    for (int i = 255; i >= 0; --i) {
        int b = (s[i / 8] >> (i & 7)) & 1;
        ed_cswap(p, q, b);
        ed_add(q, p);
        ed_add(p, p);
        ed_cswap(p, q, b);
    }
}

static void ed_scalarbase(gf p[4], const uint8_t *s) {
    gf q[4];
    gf_set(q[0], ED_X); gf_set(q[1], ED_Y); gf_set(q[2], gf1_); fmul(q[3], ED_X, ED_Y);
    ed_scalarmult(p, q, s);
}

static const int64_t ED_L[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                                  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };

/* x (64 байта-«цифры») mod L -> r[32] */
static void ed_modl(uint8_t *r, int64_t x[64]) {
    int64_t carry; int i, j;
    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * ED_L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * ED_L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; j++) x[j] -= carry * ED_L[j];
    for (i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 255);
    }
}

static void ed_reduce64(uint8_t r[64]) {
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = (int64_t)r[i];
    for (int i = 0; i < 64; i++) r[i] = 0;
    ed_modl(r, x);
}

/* распаковка точки с минусом (-A), как в TweetNaCl; -1 если это не точка кривой */
static int ed_unpackneg(gf r[4], const uint8_t p[32]) {
    gf t, chk, num, den, den2, den4, den6;
    gf_set(r[2], gf1_);
    unpack25519(r[1], p);
    fsq(num, r[1]);
    fmul(den, num, ED_D);
    fsub(num, num, r[2]);
    fadd(den, r[2], den);

    fsq(den2, den); fsq(den4, den2); fmul(den6, den4, den2);
    fmul(t, den6, num); fmul(t, t, den);
    gf_pow2523(t, t);
    fmul(t, t, num); fmul(t, t, den); fmul(t, t, den);
    fmul(r[0], t, den);

    fsq(chk, r[0]); fmul(chk, chk, den);
    if (gf_neq(chk, num)) fmul(r[0], r[0], ED_I);
    fsq(chk, r[0]); fmul(chk, chk, den);
    if (gf_neq(chk, num)) return -1;

    if (gf_par(r[0]) == (p[31] >> 7)) fsub(r[0], gf0_, r[0]);
    fmul(r[3], r[0], r[1]);
    return 0;
}

/* 0 — подпись верна. S должно быть < L (иначе подпись можно «подправить», RFC 8032 §5.1.7). */
int ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t len, const uint8_t pk[32]) {
    gf p[4], q[4];
    uint8_t t[32], h[64];
    {   /* S < L ? сравниваем с конца */
        static const uint8_t L8[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                                        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10 };
        int lt = 0;
        for (int i = 31; i >= 0; i--) {
            if (sig[32 + i] < L8[i]) { lt = 1; break; }
            if (sig[32 + i] > L8[i]) break;
        }
        if (!lt) return -1;
    }
    if (ed_unpackneg(q, pk)) return -1;
    struct sha512 c;
    sha512_init(&c);
    sha512_update(&c, sig, 32); sha512_update(&c, pk, 32); sha512_update(&c, msg, len);
    sha512_final(&c, h);
    ed_reduce64(h);
    ed_scalarmult(p, q, h);
    ed_scalarbase(q, sig + 32);
    ed_add(p, q);
    ed_pack(t, p);
    return ct_memcmp(sig, t, 32) ? -1 : 0;
}

/* ====================== для SSH: куски ChaCha20-Poly1305 в варианте OpenSSH, AES-CTR ====================== */

/* нонс OpenSSH — номер пакета, 8 байт big-endian, в последних 8 байтах 12-байтного нонса; счётчик — 32 бита в начале */
void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], const uint8_t *in, uint8_t *out, size_t len) {
    chacha_xor(key, counter, nonce, in, out, len);
}
void chacha20_keystream_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    chacha_block(key, counter, nonce, out);
}
void poly1305_mac(const uint8_t key[32], const void *data, size_t len, uint8_t tag[16]) {
    struct poly p;
    poly_init(&p, key);
    poly_update(&p, data, len);
    poly_final(&p, tag);
}

void aes128ctr_init(struct aes128ctr *c, const uint8_t key[16], const uint8_t iv[16]) {
    aes128_key((struct aes128 *)c->rk, key);
    memcpy(c->ctr, iv, 16);
    c->used = 16;
}
void aes128ctr_xor(struct aes128ctr *c, const uint8_t *in, uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (c->used == 16) {
            aes128_enc((const struct aes128 *)c->rk, c->ctr, c->ks);
            for (int k = 15; k >= 0; k--) if (++c->ctr[k]) break;      /* счётчик — 128-битное число big-endian */
            c->used = 0;
        }
        out[i] = in[i] ^ c->ks[c->used++];
    }
}


/* ====================== ECDH на NIST P-256 (secp256r1) ======================
   Нужен TLS 1.2: серверы с ECDSA-сертификатом (и IIS) выбирают P-256. Поле — Монтгомери, 4 слова по 64 бита;
   для p = 2^256 - 2^224 + 2^192 + 2^96 - 1 константа -p^-1 mod 2^64 равна 1.
   Скалярное умножение выбирает слагаемое без ветвления по биту ключа; ключ у нас одноразовый (эфемерный). */

typedef unsigned __int128 u128_;
static const uint64_t P256_P[4] = { 0xFFFFFFFFFFFFFFFFULL, 0x00000000FFFFFFFFULL, 0x0000000000000000ULL, 0xFFFFFFFF00000001ULL };
static const uint64_t P256_N[4] = { 0xF3B9CAC2FC632551ULL, 0xBCE6FAADA7179E84ULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFF00000000ULL };
static uint64_t p256_r2[4], p256_one[4], p256_b[4], p256_gx[4], p256_gy[4];
static int p256_ready;

static int u256_ge(const uint64_t a[4], const uint64_t b[4]) {
    for (int i = 3; i >= 0; i--) { if (a[i] > b[i]) return 1; if (a[i] < b[i]) return 0; }
    return 1;
}
static uint64_t u256_sub(uint64_t r[4], const uint64_t a[4], const uint64_t b[4]) {
    uint64_t br = 0;
    for (int i = 0; i < 4; i++) { u128_ d = (u128_)a[i] - b[i] - br; r[i] = (uint64_t)d; br = (uint64_t)(d >> 64) & 1; }
    return br;
}
static uint64_t u256_add(uint64_t r[4], const uint64_t a[4], const uint64_t b[4]) {
    uint64_t c = 0;
    for (int i = 0; i < 4; i++) { u128_ s = (u128_)a[i] + b[i] + c; r[i] = (uint64_t)s; c = (uint64_t)(s >> 64); }
    return c;
}
static void fe_add(uint64_t r[4], const uint64_t a[4], const uint64_t b[4]) {
    uint64_t t[4], u[4];
    uint64_t c = u256_add(t, a, b);
    uint64_t bor = u256_sub(u, t, P256_P);
    uint64_t useu = c | (bor ^ 1);                    /* была переполнена или t >= p */
    for (int i = 0; i < 4; i++) r[i] = useu ? u[i] : t[i];
}
static void fe_sub(uint64_t r[4], const uint64_t a[4], const uint64_t b[4]) {
    uint64_t t[4], u[4];
    uint64_t bor = u256_sub(t, a, b);
    u256_add(u, t, P256_P);
    for (int i = 0; i < 4; i++) r[i] = bor ? u[i] : t[i];
}
static void fe_mul(uint64_t r[4], const uint64_t a[4], const uint64_t b[4]) {
    uint64_t t[6] = { 0 };
    for (int i = 0; i < 4; i++) {
        u128_ c = 0;
        for (int j = 0; j < 4; j++) { c += (u128_)a[j] * b[i] + t[j]; t[j] = (uint64_t)c; c >>= 64; }
        c += t[4]; t[4] = (uint64_t)c; t[5] = (uint64_t)(c >> 64);
        uint64_t m = t[0];                             /* -p^-1 = 1 */
        c = (u128_)m * P256_P[0] + t[0]; c >>= 64;
        for (int j = 1; j < 4; j++) { c += (u128_)m * P256_P[j] + t[j]; t[j - 1] = (uint64_t)c; c >>= 64; }
        c += t[4]; t[3] = (uint64_t)c; t[4] = t[5] + (uint64_t)(c >> 64);
    }
    uint64_t u[4];
    uint64_t bor = u256_sub(u, t, P256_P);
    uint64_t useu = t[4] | (bor ^ 1);
    for (int i = 0; i < 4; i++) r[i] = useu ? u[i] : t[i];
}
static void fe_sqr(uint64_t r[4], const uint64_t a[4]) { fe_mul(r, a, a); }
static int fe_iszero(const uint64_t a[4]) { return (a[0] | a[1] | a[2] | a[3]) == 0; }
static int fe_eq(const uint64_t a[4], const uint64_t b[4]) { return ((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3])) == 0; }
static void fe_from_be(uint64_t r[4], const uint8_t *b) { for (int i = 0; i < 4; i++) r[3 - i] = ld64be(b + 8 * i); }
static void fe_to_be(uint8_t *b, const uint64_t a[4]) { for (int i = 0; i < 4; i++) st64be(b + 8 * i, a[3 - i]); }
static void fe_tomont(uint64_t r[4], const uint64_t a[4]) { fe_mul(r, a, p256_r2); }
static void fe_frommont(uint64_t r[4], const uint64_t a[4]) { uint64_t one[4] = { 1, 0, 0, 0 }; fe_mul(r, a, one); }
static void fe_inv(uint64_t r[4], const uint64_t a[4]) {      /* a^(p-2) */
    uint64_t e[4] = { P256_P[0] - 2, P256_P[1], P256_P[2], P256_P[3] }, acc[4], base[4];
    memcpy(acc, p256_one, 32); memcpy(base, a, 32);
    for (int i = 0; i < 256; i++) {
        if ((e[i >> 6] >> (i & 63)) & 1) fe_mul(acc, acc, base);
        fe_sqr(base, base);
    }
    memcpy(r, acc, 32);
}

static void p256_init(void) {
    if (p256_ready) return;
    static const uint8_t b_be[32] = { 0x5A,0xC6,0x35,0xD8,0xAA,0x3A,0x93,0xE7,0xB3,0xEB,0xBD,0x55,0x76,0x98,0x86,0xBC,0x65,0x1D,0x06,0xB0,0xCC,0x53,0xB0,0xF6,0x3B,0xCE,0x3C,0x3E,0x27,0xD2,0x60,0x4B };
    static const uint8_t gx_be[32] = { 0x6B,0x17,0xD1,0xF2,0xE1,0x2C,0x42,0x47,0xF8,0xBC,0xE6,0xE5,0x63,0xA4,0x40,0xF2,0x77,0x03,0x7D,0x81,0x2D,0xEB,0x33,0xA0,0xF4,0xA1,0x39,0x45,0xD8,0x98,0xC2,0x96 };
    static const uint8_t gy_be[32] = { 0x4F,0xE3,0x42,0xE2,0xFE,0x1A,0x7F,0x9B,0x8E,0xE7,0xEB,0x4A,0x7C,0x0F,0x9E,0x16,0x2B,0xCE,0x33,0x57,0x6B,0x31,0x5E,0xCE,0xCB,0xB6,0x40,0x68,0x37,0xBF,0x51,0xF5 };
    uint64_t r[4] = { 1, 0xFFFFFFFF00000000ULL, 0xFFFFFFFFFFFFFFFFULL, 0x00000000FFFFFFFEULL };   /* R mod p = 2^256 - p */
    memcpy(p256_one, r, 32);
    uint64_t x[4]; memcpy(x, r, 32);
    for (int i = 0; i < 256; i++) fe_add(x, x, x);                 /* R * 2^256 = R^2 mod p */
    memcpy(p256_r2, x, 32);
    uint64_t t[4];
    fe_from_be(t, b_be);  fe_tomont(p256_b, t);
    fe_from_be(t, gx_be); fe_tomont(p256_gx, t);
    fe_from_be(t, gy_be); fe_tomont(p256_gy, t);
    p256_ready = 1;
}

struct jp { uint64_t x[4], y[4], z[4]; };      /* z == 0 — бесконечность */

static void jp_dbl(struct jp *r, const struct jp *p) {
    if (fe_iszero(p->z)) { *r = *p; return; }
    uint64_t delta[4], gamma[4], beta[4], alpha[4], t1[4], t2[4], x3[4], y3[4], z3[4];
    fe_sqr(delta, p->z); fe_sqr(gamma, p->y); fe_mul(beta, p->x, gamma);
    fe_sub(t1, p->x, delta); fe_add(t2, p->x, delta); fe_mul(alpha, t1, t2);
    fe_add(t1, alpha, alpha); fe_add(alpha, t1, alpha);                       /* 3*(x-d)(x+d) */
    fe_sqr(x3, alpha); fe_add(t1, beta, beta); fe_add(t1, t1, t1); fe_add(t2, t1, t1);   /* t1=4β, t2=8β */
    fe_sub(x3, x3, t2);
    fe_add(z3, p->y, p->z); fe_sqr(z3, z3); fe_sub(z3, z3, gamma); fe_sub(z3, z3, delta);
    fe_sub(t1, t1, x3); fe_mul(y3, alpha, t1);
    fe_sqr(t2, gamma); fe_add(t2, t2, t2); fe_add(t2, t2, t2); fe_add(t2, t2, t2);       /* 8γ² */
    fe_sub(y3, y3, t2);
    memcpy(r->x, x3, 32); memcpy(r->y, y3, 32); memcpy(r->z, z3, 32);
}

static void jp_add(struct jp *r, const struct jp *p, const struct jp *q) {
    if (fe_iszero(p->z)) { *r = *q; return; }
    if (fe_iszero(q->z)) { *r = *p; return; }
    uint64_t z1z1[4], z2z2[4], u1[4], u2[4], s1[4], s2[4], h[4], i[4], j[4], rr[4], v[4], t[4], x3[4], y3[4], z3[4];
    fe_sqr(z1z1, p->z); fe_sqr(z2z2, q->z);
    fe_mul(u1, p->x, z2z2); fe_mul(u2, q->x, z1z1);
    fe_mul(s1, p->y, q->z); fe_mul(s1, s1, z2z2);
    fe_mul(s2, q->y, p->z); fe_mul(s2, s2, z1z1);
    fe_sub(h, u2, u1);
    fe_sub(rr, s2, s1); fe_add(rr, rr, rr);
    if (fe_iszero(h)) {
        if (fe_iszero(rr)) { jp_dbl(r, p); return; }
        memset(r, 0, sizeof *r); return;                                      /* P + (-P) */
    }
    fe_add(i, h, h); fe_sqr(i, i); fe_mul(j, h, i); fe_mul(v, u1, i);
    fe_sqr(x3, rr); fe_sub(x3, x3, j); fe_sub(x3, x3, v); fe_sub(x3, x3, v);
    fe_sub(t, v, x3); fe_mul(y3, rr, t); fe_mul(t, s1, j); fe_add(t, t, t); fe_sub(y3, y3, t);
    fe_add(z3, p->z, q->z); fe_sqr(z3, z3); fe_sub(z3, z3, z1z1); fe_sub(z3, z3, z2z2); fe_mul(z3, z3, h);
    memcpy(r->x, x3, 32); memcpy(r->y, y3, 32); memcpy(r->z, z3, 32);
}

/* scalar (32 байта big-endian) * P; 0 — результат не бесконечность */
static int jp_mul(struct jp *out, const struct jp *p, const uint8_t k[32]) {
    struct jp acc, sum;
    memset(&acc, 0, sizeof acc);
    for (int i = 255; i >= 0; i--) {
        jp_dbl(&acc, &acc);
        jp_add(&sum, &acc, p);
        uint64_t bit = (k[31 - i / 8] >> (i & 7)) & 1, mask = (uint64_t)0 - bit;
        for (int w = 0; w < 4; w++) {
            acc.x[w] = (sum.x[w] & mask) | (acc.x[w] & ~mask);
            acc.y[w] = (sum.y[w] & mask) | (acc.y[w] & ~mask);
            acc.z[w] = (sum.z[w] & mask) | (acc.z[w] & ~mask);
        }
    }
    *out = acc;
    return fe_iszero(acc.z) ? -1 : 0;
}

static void jp_affine(uint8_t x_be[32], uint8_t y_be[32], const struct jp *p) {
    uint64_t zi[4], z2[4], z3[4], ax[4], ay[4], t[4];
    fe_inv(zi, p->z); fe_sqr(z2, zi); fe_mul(z3, z2, zi);
    fe_mul(ax, p->x, z2); fe_mul(ay, p->y, z3);
    fe_frommont(t, ax); fe_to_be(x_be, t);
    if (y_be) { fe_frommont(t, ay); fe_to_be(y_be, t); }
}

int p256_pub_from_priv(uint8_t pub[65], const uint8_t priv[32]) {
    p256_init();
    struct jp g, r;
    memcpy(g.x, p256_gx, 32); memcpy(g.y, p256_gy, 32); memcpy(g.z, p256_one, 32);
    if (jp_mul(&r, &g, priv)) return -1;
    pub[0] = 4; jp_affine(pub + 1, pub + 33, &r);
    return 0;
}

/* случайный ключ в диапазоне [1, n-1] */
int p256_keygen(uint8_t priv[32], uint8_t pub[65]) {
    p256_init();
    for (int tries = 0; tries < 16; tries++) {
        rng_bytes(priv, 32);
        uint64_t k[4]; fe_from_be(k, priv);
        if (fe_iszero(k) || u256_ge(k, P256_N)) continue;
        return p256_pub_from_priv(pub, priv);
    }
    return -1;
}

/* общий секрет = x-координата priv * peer; -1 — точка не на кривой или результат вырожден */
int p256_ecdh(uint8_t out[32], const uint8_t priv[32], const uint8_t peer[65]) {
    p256_init();
    if (peer[0] != 4) return -1;
    uint64_t x[4], y[4], mx[4], my[4], l[4], r[4], t[4];
    fe_from_be(x, peer + 1); fe_from_be(y, peer + 33);
    if (u256_ge(x, P256_P) || u256_ge(y, P256_P)) return -1;
    fe_tomont(mx, x); fe_tomont(my, y);
    fe_sqr(l, my);                                             /* y^2 == x^3 - 3x + b ? */
    fe_sqr(r, mx); fe_mul(r, r, mx);
    fe_add(t, mx, mx); fe_add(t, t, mx); fe_sub(r, r, t); fe_add(r, r, p256_b);
    if (!fe_eq(l, r)) return -1;
    struct jp pt, res;
    memcpy(pt.x, mx, 32); memcpy(pt.y, my, 32); memcpy(pt.z, p256_one, 32);
    if (jp_mul(&res, &pt, priv)) return -1;
    jp_affine(out, NULL, &res);
    return 0;
}

/* ====================== генератор случайных чисел ====================== */

static uint8_t pool[32];
static uint64_t rng_ctr;
static int rng_has_rdrand = -1;

static inline uint64_t rd_tsc(void) {
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#else
    return 0;
#endif
}

static int rdrand64(uint64_t *v) {
#if defined(__x86_64__)
    if (rng_has_rdrand < 0) {
        uint32_t a = 1, b, c = 0, d;
        __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
        rng_has_rdrand = (c >> 30) & 1;
    }
    if (!rng_has_rdrand) return 0;
    for (int i = 0; i < 10; i++) {
        uint64_t x; unsigned char ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(x), "=qm"(ok) : : "cc");
        if (ok) { *v = x; return 1; }
    }
#endif
    (void)v;
    return 0;
}

void rng_add_entropy(const void *data, size_t len) {
    struct sha256 c;
    uint64_t t = rd_tsc();
    sha256_init(&c);
    sha256_update(&c, pool, 32);
    sha256_update(&c, &t, 8);
    sha256_update(&c, data, len);
    sha256_final(&c, pool);
}

void rng_bytes(void *out, size_t len) {
    uint8_t *o = out;
    while (len) {
        uint8_t blk[32], in[24];
        uint64_t hw = 0, t = rd_tsc(), ctr = ++rng_ctr;
        rdrand64(&hw);
        memcpy(in, &hw, 8); memcpy(in + 8, &t, 8); memcpy(in + 16, &ctr, 8);
        hmac_sha256(pool, 32, in, 24, blk);
        size_t k = len > 32 ? 32 : len;
        memcpy(o, blk, k);
        o += k; len -= k;
        /* продвигаем состояние, чтобы вчерашний вывод нельзя было восстановить */
        hmac_sha256(pool, 32, blk, 32, pool);
    }
}
