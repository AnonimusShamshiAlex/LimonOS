#include "http.h"
#include "net.h"
#include "tls.h"
#include "mem.h"
#include "kstring.h"
#include "format.h"


#define HDR_MAX 12288

struct conn { int tcp; struct tls *tls; int timeout; };

static int tio_send(void *ctx, const void *d, size_t n) { return tcp_send(*(int *)ctx, d, n); }
static int tio_recv(void *ctx, void *b, size_t max, int to) { return tcp_recv(*(int *)ctx, b, max, to); }

/* >0 байт, 0 таймаут, <0 конец/ошибка */
static int c_read(struct conn *c, void *buf, size_t max) {
    if (c->tls) return tls_read(c->tls, buf, max, c->timeout);
    return tcp_recv(c->tcp, buf, max, c->timeout);
}
static int c_write(struct conn *c, const void *d, size_t n) {
    return c->tls ? tls_write(c->tls, d, n) : tcp_send(c->tcp, d, n);
}

const char *http_strerror(int e) {
    switch (e) {
    case HTTP_OK: return "ok";
    case HTTP_E_URL: return "непонятный адрес";
    case HTTP_E_DNS: return "имя не нашлось (DNS)";
    case HTTP_E_CONNECT: return "не удалось подключиться";
    case HTTP_E_TLS: return "ошибка шифрованного соединения (TLS)";
    case HTTP_E_SEND: return "не удалось отправить запрос";
    case HTTP_E_TIMEOUT: return "сервер молчит (таймаут)";
    case HTTP_E_MEM: return "не хватает памяти";
    case HTTP_E_REDIRECTS: return "слишком много перенаправлений";
    case HTTP_E_PROTO: return "ответ сервера непонятен";
    case HTTP_E_TOOBIG: return "файл больше лимита";
    case HTTP_E_DOWNGRADE: return "сайт перенаправил с https на http — отказываюсь (небезопасно)";
    case HTTP_E_CLOSED: return "соединение оборвалось до конца файла";
    case HTTP_E_ENCODING: return "сервер сжал ответ (gzip/br) — пока не умеем это разжимать";
    }
    return "неизвестная ошибка";
}

void http_free(struct http_resp *r) {
    if (r && r->data) { kfree(r->data); r->data = NULL; }
    if (r) r->len = 0;
}

/* ---------- разбор адреса ---------- */
struct url { int https; char host[130]; uint16_t port; char path[1024]; };

static int url_parse(const char *s, struct url *u, int default_https) {
    memset(u, 0, sizeof *u);
    u->https = default_https;
    if (strncmp(s, "https://", 8) == 0) { u->https = 1; s += 8; }
    else if (strncmp(s, "http://", 7) == 0) { u->https = 0; s += 7; }
    size_t i = 0;
    while (*s && *s != '/' && *s != ':' && *s != '?' && *s != '#') { if (i >= sizeof u->host - 1) return -1; u->host[i++] = *s++; }
    if (!i) return -1;
    u->port = u->https ? 443 : 80;
    if (*s == ':') {
        s++; unsigned p = 0; int digits = 0;
        while (*s >= '0' && *s <= '9') { p = p * 10 + (unsigned)(*s++ - '0'); digits++; if (p > 65535) return -1; }
        if (!digits || !p) return -1;
        u->port = (uint16_t)p;
    }
    size_t o = 0;
    if (*s != '/') u->path[o++] = '/';
    while (*s && *s != '#') { if (o >= sizeof u->path - 1) return -1; u->path[o++] = *s++; }
    u->path[o] = 0;
    return 0;
}

static void url_format(const struct url *u, char *out, size_t n) {
    int def = u->https ? 443 : 80;
    if (u->port == def) ksnprintf(out, n, "%s://%s%s", u->https ? "https" : "http", u->host, u->path);
    else ksnprintf(out, n, "%s://%s:%u%s", u->https ? "https" : "http", u->host, (unsigned)u->port, u->path);
}

/* относительный Location -> новый url */
static int url_join(const struct url *base, const char *loc, struct url *out) {
    if (strncmp(loc, "http://", 7) == 0 || strncmp(loc, "https://", 8) == 0) return url_parse(loc, out, base->https);
    *out = *base;
    if (loc[0] == '/' && loc[1] == '/') { char tmp[1200]; ksnprintf(tmp, sizeof tmp, "%s:%s", base->https ? "https" : "http", loc); return url_parse(tmp, out, base->https); }
    if (loc[0] == '/') { size_t ln = strlen(loc); if (ln >= sizeof out->path) return -1; memcpy(out->path, loc, ln + 1); return 0; }
    /* относительный путь: заменяем последний сегмент */
    char tmp[1100]; size_t n = strlen(base->path);
    while (n > 0 && base->path[n - 1] != '/') n--;
    if (n + strlen(loc) >= sizeof tmp) return -1;
    memcpy(tmp, base->path, n); memcpy(tmp + n, loc, strlen(loc) + 1);
    memcpy(out->path, tmp, strlen(tmp) + 1);
    return 0;
}

/* ---------- заголовки ---------- */
static int ieq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}

static int hdr_get(const char *h, size_t hl, const char *name, char *out, size_t outmax) {
    size_t nl = strlen(name);
    size_t i = 0;
    while (i < hl) {
        size_t e = i; while (e < hl && h[e] != '\n') e++;
        if (e - i > nl && h[i + nl] == ':' && ieq_n(h + i, name, nl)) {
            size_t v = i + nl + 1; while (v < e && (h[v] == ' ' || h[v] == '\t')) v++;
            size_t o = 0; while (v < e && h[v] != '\r' && o + 1 < outmax) out[o++] = h[v++];
            out[o] = 0; return 0;
        }
        i = e + 1;
    }
    return -1;
}

/* ---------- накопитель тела ---------- */
struct sink { uint8_t *d; size_t len, cap, max, total; void (*prog)(size_t, size_t, void *); void *pctx; int err; };

static int sink_put(struct sink *s, const uint8_t *p, size_t n) {
    if (s->err) return s->err;
    if (s->len + n > s->max) { s->err = HTTP_E_TOOBIG; return s->err; }
    if (s->len + n + 1 > s->cap) {
        size_t nc = s->cap ? s->cap : 65536;
        while (nc < s->len + n + 1) nc *= 2;
        if (nc > s->max + 1) nc = s->max + 1;
        uint8_t *nd = kmalloc(nc);
        if (!nd) { s->err = HTTP_E_MEM; return s->err; }
        if (s->d) { memcpy(nd, s->d, s->len); kfree(s->d); }
        s->d = nd; s->cap = nc;
    }
    memcpy(s->d + s->len, p, n); s->len += n; s->d[s->len] = 0;
    if (s->prog) s->prog(s->len, s->total, s->pctx);
    return 0;
}

/* ---------- один запрос (без переходов) ---------- */
struct one {
    int status; char location[400]; char ctype[80];
    int https, tls_err; char cipher[40]; uint8_t cert[32]; int have_cert;
};

static int do_one(const struct url *u, const struct http_opts *o, struct sink *sink, struct one *r) {
    memset(r, 0, sizeof *r);
    uint32_t ip;
    if (net_resolve(u->host, &ip) != 0) return HTTP_E_DNS;
    struct conn c = { -1, NULL, o->timeout_ms > 0 ? o->timeout_ms : 8000 };
    c.tcp = tcp_connect(ip, u->port, 6000);
    if (c.tcp < 0) return HTTP_E_CONNECT;
    int rc = HTTP_OK;
    uint8_t *hb = NULL; char *req = NULL;
    if (u->https) {
        struct tls_io io = { tio_send, tio_recv, &c.tcp };
        int e = 0;
        c.tls = tls_open(&io, u->host, &e);
        if (!c.tls) { r->tls_err = e; rc = HTTP_E_TLS; goto out; }
        const struct tls_info *ti = tls_info(c.tls);
        r->https = 1;
        memcpy(r->cipher, ti->cipher, strlen(ti->cipher) + 1);
        memcpy(r->cert, ti->cert_sha256, 32); r->have_cert = ti->have_cert;
    }
    const char *method = o->method ? o->method : "GET";
    req = kmalloc(2048); hb = kmalloc(HDR_MAX);
    if (!req || !hb) { rc = HTTP_E_MEM; goto out; }
    int hostport_default = u->port == (u->https ? 443 : 80);
    int rn = ksnprintf(req, 2048, "%s %s HTTP/1.1\r\nHost: %s", method, u->path, u->host);
    if (!hostport_default) rn += ksnprintf(req + rn, 2048 - (size_t)rn, ":%u", (unsigned)u->port);
    rn += ksnprintf(req + rn, 2048 - (size_t)rn, "\r\nUser-Agent: LimonOS/0.6\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n");
    if (o->body_len || (o->body && o->content_type)) {
        rn += ksnprintf(req + rn, 2048 - (size_t)rn, "Content-Type: %s\r\nContent-Length: %u\r\n",
                        o->content_type ? o->content_type : "application/octet-stream", (unsigned)o->body_len);
    }
    if (o->extra_headers && rn > 0) rn += ksnprintf(req + rn, 2048 - (size_t)rn, "%s", o->extra_headers);
    rn += ksnprintf(req + rn, 2048 - (size_t)rn, "\r\n");
    if (rn <= 0 || rn >= 2048) { rc = HTTP_E_URL; goto out; }
    if (c_write(&c, req, (size_t)rn) != 0) { rc = HTTP_E_SEND; goto out; }
    if (o->body_len && c_write(&c, o->body, o->body_len) != 0) { rc = HTTP_E_SEND; goto out; }

    /* --- заголовки ответа --- */
    size_t hl = 0; ptrdiff_t hend = -1;
    while (hend < 0) {
        if (hl >= HDR_MAX) { rc = HTTP_E_PROTO; goto out; }
        int n = c_read(&c, hb + hl, HDR_MAX - hl);
        if (n == 0) {
#ifdef LIMON_HOSTTEST
            extern void tcp_debug_dump(void); tcp_debug_dump();
#endif
            rc = HTTP_E_TIMEOUT; goto out; }
        if (n < 0) { rc = hl ? HTTP_E_CLOSED : HTTP_E_CONNECT; goto out; }
        size_t from = hl > 3 ? hl - 3 : 0;
        hl += (size_t)n;
        for (size_t i = from; i + 3 < hl; i++)
            if (hb[i] == '\r' && hb[i + 1] == '\n' && hb[i + 2] == '\r' && hb[i + 3] == '\n') { hend = (ptrdiff_t)i + 4; break; }
    }
    const char *h = (const char *)hb;
    if (hend < 12 || memcmp(h, "HTTP/1.", 7) != 0) { rc = HTTP_E_PROTO; goto out; }
    r->status = (h[9] - '0') * 100 + (h[10] - '0') * 10 + (h[11] - '0');
    char tmp[64];
    hdr_get(h, (size_t)hend, "Location", r->location, sizeof r->location);
    hdr_get(h, (size_t)hend, "Content-Type", r->ctype, sizeof r->ctype);
    int chunked = hdr_get(h, (size_t)hend, "Transfer-Encoding", tmp, sizeof tmp) == 0 && ieq_n(tmp, "chunked", 7);
    char enc[32];
    if (hdr_get(h, (size_t)hend, "Content-Encoding", enc, sizeof enc) == 0 && !ieq_n(enc, "identity", 8)) {
        /* мы просили Accept-Encoding: identity, но некоторые серверы всё равно сжимают (например редиректы без тела —
           там это не страшно, проверяем только когда дальше реально будет тело) */
        if (!(r->status >= 300 && r->status < 400 && r->location[0])) { rc = HTTP_E_ENCODING; goto out; }
    }
    long clen = -1;
    if (hdr_get(h, (size_t)hend, "Content-Length", tmp, sizeof tmp) == 0) {
        clen = 0; for (const char *p = tmp; *p >= '0' && *p <= '9'; p++) clen = clen * 10 + (*p - '0');
    }
    int no_body = (strcmp(method, "HEAD") == 0) || r->status == 204 || r->status == 304 || (r->status >= 100 && r->status < 200);
    int is_redirect = r->status == 301 || r->status == 302 || r->status == 303 || r->status == 307 || r->status == 308;
    if (no_body || (is_redirect && r->location[0])) goto out;       /* тело перехода нам не нужно */
    sink->total = clen > 0 ? (size_t)clen : 0;
    if (clen > 0 && (size_t)clen > sink->max) { rc = HTTP_E_TOOBIG; goto out; }

    uint8_t *rest = hb + hend; size_t restn = hl - (size_t)hend;
    uint8_t *buf = hb;                    /* переиспользуем буфер заголовков для чтения тела: сначала обработаем остаток */
    /* копируем остаток в начало, чтобы читать дальше в тот же буфер */
    memmove(buf, rest, restn);
    size_t have = restn;
    long remaining = clen;
    int cs = 0; size_t chunk_left = 0; int line_len = 0; int done = 0;
    for (;;) {
        if (have) {
            size_t i = 0;
            if (chunked) {
                while (i < have && !done) {
                    uint8_t ch = buf[i];
                    if (cs == 0) {                                 /* размер чанка */
                        if (ch == '\n') { cs = chunk_left ? 1 : 3; line_len = 0; }
                        else if (ch >= '0' && ch <= '9') chunk_left = chunk_left * 16 + (size_t)(ch - '0');
                        else if (ch >= 'a' && ch <= 'f') chunk_left = chunk_left * 16 + (size_t)(ch - 'a' + 10);
                        else if (ch >= 'A' && ch <= 'F') chunk_left = chunk_left * 16 + (size_t)(ch - 'A' + 10);
                        else if (ch == ';') cs = 4;                 /* расширение чанка — пропускаем до конца строки */
                        i++;
                    } else if (cs == 4) { if (ch == '\n') cs = chunk_left ? 1 : 3; i++; }
                    else if (cs == 1) {                             /* данные */
                        size_t k = have - i; if (k > chunk_left) k = chunk_left;
                        if (sink_put(sink, buf + i, k)) { rc = sink->err; goto out; }
                        i += k; chunk_left -= k;
                        if (!chunk_left) cs = 2;
                    } else if (cs == 2) { if (ch == '\n') cs = 0; i++; }   /* CRLF после чанка */
                    else {                                           /* хвост после последнего чанка */
                        if (ch == '\n') { if (line_len == 0) done = 1; line_len = 0; }
                        else if (ch != '\r') line_len++;
                        i++;
                    }
                }
            } else {
                size_t k = have;
                if (remaining >= 0 && (size_t)remaining < k) k = (size_t)remaining;
                if (sink_put(sink, buf, k)) { rc = sink->err; goto out; }
                if (remaining >= 0) { remaining -= (long)k; if (remaining == 0) done = 1; }
            }
            have = 0;
        }
        if (done) break;
        int n = c_read(&c, buf, HDR_MAX);
        if (n == 0) {
#ifdef LIMON_HOSTTEST
            extern void tcp_debug_dump(void); tcp_debug_dump();
#endif
            rc = HTTP_E_TIMEOUT; goto out; }
        if (n < 0) {
            /* конец потока: нормально только если длина заранее не известна */
            if (chunked || remaining > 0) rc = HTTP_E_CLOSED;
            break;
        }
        have = (size_t)n;
    }
out:
    if (hb) kfree(hb);
    if (req) kfree(req);
    if (c.tls) tls_close(c.tls);
    tcp_close(c.tcp);
    return rc;
}

static int fetch_scheme(struct url *u, const struct http_opts *o, struct http_resp *out) {
    struct http_opts opt = *o;
    if (!opt.max_bytes) opt.max_bytes = 16u << 20;
    int rc = 0;
    int redirects = 0, max_redir = o->max_redirects > 0 ? o->max_redirects : 6;
    const char *orig_method = o->method ? o->method : "GET";
    for (;;) {
        struct sink sk; memset(&sk, 0, sizeof sk);
        sk.max = opt.max_bytes; sk.prog = o->progress; sk.pctx = o->pctx;
        struct one one;
        rc = do_one(u, &opt, &sk, &one);
        if (rc == HTTP_OK && one.status >= 301 && one.status <= 308 && one.status != 304 && one.status != 305 && one.status != 306 && one.location[0]) {
            if (sk.d) kfree(sk.d);
            if (++redirects > max_redir) return HTTP_E_REDIRECTS;
            struct url nu;
            if (url_join(u, one.location, &nu) != 0) return HTTP_E_URL;
            if (u->https && !nu.https) return HTTP_E_DOWNGRADE;     /* из шифрованного в открытый — не идём */
            if (one.status == 301 || one.status == 302 || one.status == 303) { opt.method = strcmp(orig_method, "HEAD") == 0 ? "HEAD" : "GET"; opt.body = NULL; opt.body_len = 0; }
            *u = nu;
            continue;
        }
        out->status = one.status; out->https = one.https; out->tls_err = one.tls_err;
        memcpy(out->cipher, one.cipher, sizeof out->cipher);
        memcpy(out->cert_sha256, one.cert, 32); out->have_cert = one.have_cert;
        memcpy(out->content_type, one.ctype, sizeof out->content_type);
        out->redirects = redirects;
        url_format(u, out->url, sizeof out->url);
        if (rc != HTTP_OK) { if (sk.d) kfree(sk.d); return rc; }
        if (!sk.d) { sk.d = kmalloc(16); if (!sk.d) return HTTP_E_MEM; sk.d[0] = 0; }
        out->data = sk.d; out->len = sk.len;
        return HTTP_OK;
    }
}

int http_fetch(const char *url, const struct http_opts *opts, struct http_resp *out) {
    static const struct http_opts defaults;
    if (!opts) opts = &defaults;
    memset(out, 0, sizeof *out);
    struct url u;
    int has_scheme = strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0;
    if (url_parse(url, &u, 1) != 0) return HTTP_E_URL;
    int rc = fetch_scheme(&u, opts, out);
    if (!has_scheme && (rc == HTTP_E_CONNECT || (rc == HTTP_E_TLS && out->tls_err == TLS_E_UNSUPPORTED))) {
        /* адрес без http/https: https не вышел — пробуем обычный http */
        http_free(out);
        if (url_parse(url, &u, 0) != 0) return HTTP_E_URL;
        rc = fetch_scheme(&u, opts, out);
    }
    return rc;
}
