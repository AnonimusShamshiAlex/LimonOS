#ifndef LIMON_TLS_H
#define LIMON_TLS_H
#include <stdint.h>
#include <stddef.h>

/* Seal — TLS 1.3 клиент LimonOS (https).
   Наборы 1.3: TLS_CHACHA20_POLY1305_SHA256, TLS_AES_128_GCM_SHA256. Обмен ключами: X25519.
   Запасной вариант — TLS 1.2: ECDHE (X25519) + ChaCha20-Poly1305 / AES-128-GCM, extended master secret,
   возобновление сессии по её id.
   ВАЖНО: на этом этапе шифрование и целостность работают, а подлинность сервера
   (цепочка сертификатов) ещё НЕ проверяется — см. tls_info()->authenticated. */

/* Транспорт: tls ничего не знает про TCP, поэтому его легко тестировать. */
struct tls_io {
    int (*send)(void *ctx, const void *data, size_t len);                 /* 0 — успех */
    int (*recv)(void *ctx, void *buf, size_t max, int timeout_ms);        /* >0 байт, 0 таймаут, <0 соединение закрыто */
    void *ctx;
};

enum {
    TLS_OK = 0,
    TLS_E_IO = -1,        /* транспорт оборвался */
    TLS_E_PROTO = -2,     /* сервер нарушил протокол */
    TLS_E_CRYPTO = -3,    /* не сошлась подпись/тег — данные подделаны или повреждены */
    TLS_E_ALERT = -4,     /* сервер прислал ошибку (alert) */
    TLS_E_UNSUPPORTED = -5,
    TLS_EOF = -6,         /* сервер корректно закрыл соединение (close_notify) */
    TLS_E_MEM = -7,
    TLS_E_TIMEOUT = -8
};

struct tls;   /* непрозрачная структура */

/* Флаги tls_open_ex */
#define TLS_F_ONLY12  1   /* предлагать только TLS 1.2 (нужно FTPS: vsftpd/ProFTPD требуют общую сессию на канале данных) */

/* Сессия TLS 1.2 для возобновления (канал данных FTPS «переиспользует» сессию канала управления) */
struct tls_session {
    int valid;
    uint8_t id[32]; uint8_t idlen;
    uint8_t master[48];
    uint16_t suite;
    int ems;
};

struct tls_info {
    const char *cipher;           /* название набора шифров */
    uint8_t cert_sha256[32];      /* отпечаток сертификата сервера (первого в цепочке) */
    int have_cert;
    int authenticated;            /* 1 — цепочка сертификатов проверена (пока всегда 0) */
    int alert;                    /* код alert от сервера, если был */
    int version;                  /* 12 или 13 */
    int resumed;                  /* 1 — сессия возобновлена без полного рукопожатия */
};

struct tls *tls_open(const struct tls_io *io, const char *host, int *err);  /* рукопожатие; NULL при ошибке, код в *err */
/* то же с флагами; resume (может быть NULL) — сессия от tls_session_save прошлого соединения с тем же сервером */
struct tls *tls_open_ex(const struct tls_io *io, const char *host, int flags, const struct tls_session *resume, int *err);
int   tls_session_save(const struct tls *t, struct tls_session *out);   /* 0 — сохранили (только TLS 1.2 с id сессии) */
int   tls_write(struct tls *t, const void *data, size_t len);               /* 0 — успех */
int   tls_read(struct tls *t, void *buf, size_t max, int timeout_ms);       /* >0 байт, 0 таймаут, <0 ошибка/TLS_EOF */
void  tls_close(struct tls *t);                                             /* шлёт close_notify и освобождает память */
const struct tls_info *tls_info(const struct tls *t);
const char *tls_strerror(int err);

#endif
