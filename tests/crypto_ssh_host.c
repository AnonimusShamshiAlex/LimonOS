/* Хост-помощник для tests/test_crypto_ssh.py: гоняет crypto.c (SHA-512, Ed25519, ChaCha20/Poly1305 OpenSSH, AES-CTR). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crypto.h"

static size_t unhex(const char *s, uint8_t *o) { size_t n = strlen(s) / 2; for (size_t i = 0; i < n; i++) { unsigned v; sscanf(s + 2 * i, "%2x", &v); o[i] = (uint8_t)v; } return n; }
static void hex(const uint8_t *d, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", d[i]); printf("\n"); }

int main(int argc, char **argv) {
    (void)argc;
    static uint8_t a[1 << 16], b[1 << 16], c[1 << 16], o[1 << 16];
    if (!strcmp(argv[1], "sha512")) { size_t n = unhex(argv[2], a); uint8_t h[64]; sha512(a, n, h); hex(h, 64); }
    else if (!strcmp(argv[1], "ed25519")) { /* sig pk msg */
        unhex(argv[2], a); unhex(argv[3], b); size_t n = unhex(argv[4], c);
        printf("%d\n", ed25519_verify(a, c, n, b));
    } else if (!strcmp(argv[1], "chacha")) { /* key counter nonce12 data */
        unhex(argv[2], a); unhex(argv[4], b); size_t n = unhex(argv[5], c);
        chacha20_xor(a, (uint32_t)atoi(argv[3]), b, c, o, n); hex(o, n);
    } else if (!strcmp(argv[1], "poly")) { unhex(argv[2], a); size_t n = unhex(argv[3], b); uint8_t t[16]; poly1305_mac(a, b, n, t); hex(t, 16); }
    else if (!strcmp(argv[1], "aesctr")) { /* key iv data (двумя кусками, чтобы проверить состояние) */
        unhex(argv[2], a); unhex(argv[3], b); size_t n = unhex(argv[4], c);
        struct aes128ctr x; aes128ctr_init(&x, a, b);
        size_t h = n / 3; aes128ctr_xor(&x, c, o, h); aes128ctr_xor(&x, c + h, o + h, n - h); hex(o, n);
    }
    else if (!strcmp(argv[1], "p256pub")) { unhex(argv[2], a); uint8_t pub[65]; printf("%d ", p256_pub_from_priv(pub, a)); hex(pub, 65); }
    else if (!strcmp(argv[1], "p256ecdh")) { unhex(argv[2], a); unhex(argv[3], b); uint8_t out[32]; int r = p256_ecdh(out, a, b); printf("%d ", r); hex(out, 32); }
    return 0;
}
