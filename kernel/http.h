#ifndef LIMON_HTTP_H
#define LIMON_HTTP_H
#include <stdint.h>
#include <stddef.h>

/* Honey — HTTP/HTTPS клиент LimonOS: https (TLS 1.3), переходы по ссылкам (redirect),
   chunked, Content-Length, скачивание в память, отправка (POST/PUT). */

enum {
    HTTP_OK = 0,
    HTTP_E_URL = -1, HTTP_E_DNS = -2, HTTP_E_CONNECT = -3, HTTP_E_TLS = -4, HTTP_E_SEND = -5,
    HTTP_E_TIMEOUT = -6, HTTP_E_MEM = -7, HTTP_E_REDIRECTS = -8, HTTP_E_PROTO = -9,
    HTTP_E_TOOBIG = -10, HTTP_E_DOWNGRADE = -11, HTTP_E_CLOSED = -12, HTTP_E_ENCODING = -13
};

struct http_opts {
    const char *method;          /* "GET" по умолчанию */
    const void *body; size_t body_len;
    const char *content_type;    /* для POST/PUT */
    size_t max_bytes;            /* лимит тела, по умолчанию 16 МиБ */
    int max_redirects;           /* по умолчанию 6 */
    int timeout_ms;              /* ожидание данных, по умолчанию 8000 */
    void (*progress)(size_t got, size_t total, void *ctx);   /* total = 0, если размер неизвестен */
    void *pctx;
    const char *extra_headers;   /* дополнительные заголовки целиком, каждый уже с "Имя: значение\r\n" (для API-ключей и т.п.) */
};

struct http_resp {
    int status;
    uint8_t *data;               /* тело ответа (kmalloc), в конце всегда добавлен нулевой байт; освободить http_free() */
    size_t len;
    char url[320];               /* итоговый адрес после переходов */
    char content_type[80];
    int https;
    int tls_err;                 /* код TLS при HTTP_E_TLS */
    char cipher[40];
    uint8_t cert_sha256[32];
    int have_cert, authenticated;
    int redirects;
};

int  http_fetch(const char *url, const struct http_opts *opts, struct http_resp *out);   /* 0 или HTTP_E_* */
void http_free(struct http_resp *r);
const char *http_strerror(int err);

#endif
