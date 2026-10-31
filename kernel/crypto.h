#ifndef LIMON_CRYPTO_H
#define LIMON_CRYPTO_H
#include <stdint.h>
#include <stddef.h>

/* Squeeze — криптография LimonOS. Всё написано с нуля, без SSE и без libc.
   Нужна для TLS 1.3 (https). */

/* ---- SHA-256 ---- */
struct sha256 { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; };
void sha256_init(struct sha256 *c);
void sha256_update(struct sha256 *c, const void *data, size_t len);
void sha256_final(struct sha256 *c, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);

/* ---- HMAC-SHA256 и HKDF ---- */
void hmac_sha256(const uint8_t *key, size_t klen, const void *data, size_t len, uint8_t out[32]);
void hkdf_extract(const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen, uint8_t prk[32]);
void hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t ilen, uint8_t *out, size_t olen);
/* HKDF-Expand-Label из TLS 1.3 (метка без префикса "tls13 ") */
void tls13_expand_label(const uint8_t secret[32], const char *label,
                        const uint8_t *ctx, size_t clen, uint8_t *out, size_t olen);

/* ---- ChaCha20-Poly1305 (RFC 8439) ---- */
void chacha20poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, size_t aadlen,
                           const uint8_t *pt, size_t len, uint8_t *ct, uint8_t tag[16]);
int  chacha20poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, size_t aadlen,
                           const uint8_t *ct, size_t len, const uint8_t tag[16], uint8_t *pt);  /* 0 — тег верный */

/* ---- AES-128-GCM ---- */
void aes128gcm_seal(const uint8_t key[16], const uint8_t nonce[12],
                    const uint8_t *aad, size_t aadlen,
                    const uint8_t *pt, size_t len, uint8_t *ct, uint8_t tag[16]);
int  aes128gcm_open(const uint8_t key[16], const uint8_t nonce[12],
                    const uint8_t *aad, size_t aadlen,
                    const uint8_t *ct, size_t len, const uint8_t tag[16], uint8_t *pt);

/* ---- X25519 (RFC 7748) ---- */
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);


/* ---- SHA-512 ---- */
struct sha512 { uint64_t h[8]; uint64_t len; uint8_t buf[128]; size_t n; };
void sha512_init(struct sha512 *c);
void sha512_update(struct sha512 *c, const void *data, size_t len);
void sha512_final(struct sha512 *c, uint8_t out[64]);
void sha512(const void *data, size_t len, uint8_t out[64]);

/* ---- Ed25519: проверка подписи (RFC 8032), 0 — подпись верна ---- */
int  ed25519_verify(const uint8_t sig[64], const uint8_t *msg, size_t len, const uint8_t pk[32]);

/* ---- куски для SSH (chacha20-poly1305@openssh.com, aes128-ctr) ---- */
void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], const uint8_t *in, uint8_t *out, size_t len);
void chacha20_keystream_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]);
void poly1305_mac(const uint8_t key[32], const void *data, size_t len, uint8_t tag[16]);

struct aes128ctr { uint8_t rk[176]; uint8_t ctr[16]; uint8_t ks[16]; unsigned used; };
void aes128ctr_init(struct aes128ctr *c, const uint8_t key[16], const uint8_t iv[16]);
void aes128ctr_xor(struct aes128ctr *c, const uint8_t *in, uint8_t *out, size_t len);

/* ---- ECDH P-256: pub — несжатая точка (0x04 || X || Y), 65 байт; 0 — успех ---- */
int  p256_keygen(uint8_t priv[32], uint8_t pub[65]);
int  p256_pub_from_priv(uint8_t pub[65], const uint8_t priv[32]);
int  p256_ecdh(uint8_t out[32], const uint8_t priv[32], const uint8_t peer[65]);

/* ---- случайные числа ---- */
void rng_add_entropy(const void *data, size_t len);   /* подмешать что-то непредсказуемое (тики, мышь, MAC) */
void rng_bytes(void *out, size_t len);

/* сравнение без утечки по времени, 0 — одинаково */
int ct_memcmp(const void *a, const void *b, size_t n);

#endif
