#include "ftp.h"
#include "tls.h"
#include "crypto.h"
#include "format.h"
#include "kstring.h"
#include "mem.h"
#include <stdarg.h>

#define CTL_TO     8000
#define LIST_MAX   (2u << 20)
#define GET_MAX    (8u << 20)

static char *memchr_(char *b, size_t n) { for (size_t i = 0; i < n; i++) if (b[i] == '\n') return b + i; return NULL; }

static void say(const struct ftp_ops *o, int kind, const char *fmt, ...) {
    char b[400];
    va_list ap; va_start(ap, fmt); kvsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (o->say) o->say(o->ud, kind, b);
}

static void fail(struct ftp_result *r, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); kvsnprintf(r->msg, sizeof r->msg, fmt, ap); va_end(ap);
}

/* ---- канал: открытый TCP или TLS поверх него ---- */
static int ch_send(struct ftp_chan *c, const void *d, size_t n) { return c->tls ? tls_write(c->tls, d, n) : c->ops->send(c->h, d, n); }
static int ch_recv(struct ftp_chan *c, void *b, size_t max, int to) { return c->tls ? tls_read(c->tls, b, max, to) : c->ops->recv(c->h, b, max, to); }
static void ch_close(struct ftp_chan *c) {
    if (c->tls) { tls_close(c->tls); c->tls = NULL; }
    if (c->h) { c->ops->close(c->h); c->h = NULL; }
}

/* ---- ответы сервера: строки «NNN текст»; многострочный — «NNN-...» до строки «NNN ...» с тем же кодом ---- */
/* возвращает код (100..599), -1 таймаут, -2 соединение закрыто */
static int read_reply(struct ftp_session *c, char *text, size_t cap, int timeout_ms) {
    int code = 0, multi = 0, waited = 0;
    if (text && cap) text[0] = 0;
    for (;;) {
        char *nl = memchr_(c->buf, c->len);
        if (nl) {
            size_t ll = (size_t)(nl - c->buf);
            char line[512]; size_t k = ll < sizeof line - 1 ? ll : sizeof line - 1;
            memcpy(line, c->buf, k);
            while (k && line[k - 1] == '\r') k--;
            line[k] = 0;
            memmove(c->buf, nl + 1, c->len - ll - 1);
            c->len -= ll + 1;
            say(c->ch.ops, FTP_SAY_REPLY, "%s", line);
            int is_code = k >= 3 && line[0] >= '0' && line[0] <= '9' && line[1] >= '0' && line[1] <= '9' && line[2] >= '0' && line[2] <= '9';
            if (is_code) {
                int lc = (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
                char sep = k > 3 ? line[3] : ' ';
                if (!multi) {
                    code = lc;
                    if (sep == '-') multi = 1;
                }
                if (lc == code && sep != '-') {                    /* конец ответа */
                    if (text && cap) ksnprintf(text, cap, "%s", k > 4 ? line + 4 : "");
                    return code;
                }
                if (multi && text && cap && lc == code) ksnprintf(text, cap, "%s", k > 4 ? line + 4 : "");
            }
            continue;
        }
        if (c->len == sizeof c->buf) c->len = 0;                   /* мусорная строка длиннее буфера — выбрасываем */
        int n = ch_recv(&c->ch, c->buf + c->len, sizeof c->buf - c->len, 1000);
        if (n > 0) { c->len += (size_t)n; waited = 0; continue; }
        if (n < 0) return -2;
        waited += 1000;
        if (waited >= timeout_ms) return -1;
    }
}

static int send_cmd(struct ftp_session *c, const char *verb, const char *arg) {
    char cmdbuf[600];
    if (arg && (strchr(arg, '\r') || strchr(arg, '\n'))) return -1;            /* защита от подстановки команд */
    if (arg) ksnprintf(cmdbuf, sizeof cmdbuf, "%s %s\r\n", verb, arg); else ksnprintf(cmdbuf, sizeof cmdbuf, "%s\r\n", verb);
    if (strcmp(verb, "PASS") == 0) say(c->ch.ops, FTP_SAY_CMD, "PASS ********");
    else { size_t l = strlen(cmdbuf); char t[600]; memcpy(t, cmdbuf, l - 2); t[l - 2] = 0; say(c->ch.ops, FTP_SAY_CMD, "%s", t); }
    return ch_send(&c->ch, cmdbuf, strlen(cmdbuf));
}

/* команда и ответ одним вызовом */
static int cmd(struct ftp_session *c, const char *verb, const char *arg, char *text, size_t cap) {
    if (send_cmd(c, verb, arg)) return -2;
    return read_reply(c, text, cap, CTL_TO);
}

static const char *tls_open_msg(int e) { return tls_strerror(e); }

static void hex_fp(const uint8_t *d, char *out) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = hx[d[i] >> 4]; out[2 * i + 1] = hx[d[i] & 15]; }
    out[64] = 0;
}

static void report_tls(const struct ftp_ops *o, const char *what, const struct tls *t) {
    const struct tls_info *ti = tls_info(t);
    say(o, FTP_SAY_INFO, "%s: TLS 1.%d, %s%s", what, ti->version == 13 ? 3 : 2, ti->cipher, ti->resumed ? ", сессия возобновлена" : "");
    if (ti->have_cert && strcmp(what, "Канал управления") == 0) {
        char fp[65]; hex_fp(ti->cert_sha256, fp);
        say(o, FTP_SAY_INFO, "Сертификат сервера, SHA-256 (подлинность НЕ проверена, сверь сам):");
        say(o, FTP_SAY_INFO, "  %s", fp);
    }
}

static int parse_pasv(const char *s, uint16_t *port) {
    const char *p = s;
    while (*p && *p != '(') p++;
    if (!*p) { p = s; while (*p && (*p < '0' || *p > '9')) p++; } else p++;     /* некоторые серверы пишут без скобок */
    unsigned v[6];
    for (int i = 0; i < 6; i++) {
        unsigned n = 0; int any = 0;
        while (*p >= '0' && *p <= '9') { n = n * 10 + (unsigned)(*p++ - '0'); any = 1; if (n > 255 && i < 4) return -1; }
        if (!any || n > 255) return -1;
        v[i] = n;
        if (i < 5) { if (*p != ',') return -1; p++; }
    }
    *port = (uint16_t)((v[4] << 8) | v[5]);
    return 0;
}

static int parse_epsv(const char *s, uint16_t *port) {
    const char *p = s;
    while (*p && *p != '(') p++;
    if (!*p) return -1;
    char d = p[1];
    if (!d || p[2] != d || p[3] != d) return -1;
    p += 4;
    unsigned n = 0; int any = 0;
    while (*p >= '0' && *p <= '9') { n = n * 10 + (unsigned)(*p++ - '0'); any = 1; if (n > 65535) return -1; }
    if (!any || *p != d || n == 0) return -1;
    *port = (uint16_t)n;
    return 0;
}

static int safe_arg(const char *s) { return s && !strchr(s, '\r') && !strchr(s, '\n'); }

static int is_tls_error_retryable(int e) { return e == TLS_E_ALERT || e == TLS_E_UNSUPPORTED || e == TLS_E_PROTO; }

/* ---------- подключение БЕЗ логина: dial, TLS (если надо), баннер 220, AUTH TLS/PBSZ/PROT.
   Как ftp.exe: после этого сервер ждёт USER, а имя и пароль спрашивает уже пользователь. ---------- */
static int do_open(const struct ftp_ops *o, const struct ftp_req *rq, struct ftp_session *fs, struct ftp_result *res, int tls_flags) {
    memset(fs, 0, sizeof *fs);
    fs->ch.ops = o;
    fs->host = rq->host; fs->ip = rq->ip;
    char text[300];
    int code, derr = 0;

    fs->ch.h = o->dial(o->ud, rq->ip, rq->port, 3000, &derr);
    if (!fs->ch.h) {
        fail(res, "%s", derr == -3 ? "Порт закрыт (RST)." : derr == -2 ? "Сервер не отвечает." : "Не удалось подключиться.");
        return FTP_E_NET;
    }
    struct tls_io io = { o->send, o->recv, fs->ch.h };

    if (rq->implicit) {
        int e;
        fs->ch.tls = tls_open_ex(&io, rq->host, tls_flags, NULL, &e);
        if (!fs->ch.tls) {
            int rc = ((tls_flags & TLS_F_ONLY12) && is_tls_error_retryable(e)) ? FTP_E_TLSVER : FTP_E_TLS;
            if (rc != FTP_E_TLSVER) fail(res, "TLS: %s", tls_open_msg(e));
            ch_close(&fs->ch); return rc;
        }
        fs->control_tls = 1;
        report_tls(o, "Канал управления", fs->ch.tls);
    }

    code = read_reply(fs, text, sizeof text, CTL_TO);
    if (code == 120) code = read_reply(fs, text, sizeof text, 60000);
    if (code != 220) {
        res->code = code; fail(res, "Сервер не поздоровался (код %d). %s", code, code > 0 ? text : "Нет ответа.");
        ch_close(&fs->ch); return FTP_E_NET;
    }

    if (!rq->implicit && rq->sec != FTP_SEC_PLAIN) {
        code = cmd(fs, "AUTH", "TLS", text, sizeof text);
        if (code == 234) {
            int e;
            fs->ch.tls = tls_open_ex(&io, rq->host, tls_flags, NULL, &e);
            if (!fs->ch.tls) {
                int rc = ((tls_flags & TLS_F_ONLY12) && is_tls_error_retryable(e)) ? FTP_E_TLSVER : FTP_E_TLS;
                if (rc != FTP_E_TLSVER) fail(res, "Рукопожатие TLS не удалось: %s", tls_open_msg(e));
                ch_close(&fs->ch); return rc;
            }
            fs->control_tls = 1;
            report_tls(o, "Канал управления", fs->ch.tls);
        } else if (rq->sec == FTP_SEC_TLS) {
            res->code = code;
            fail(res, "Сервер не поддерживает шифрование (AUTH TLS → %d). %s", code, code > 0 ? text : "");
            ch_close(&fs->ch); return FTP_E_NEEDTLS;
        } else if (code < 0) {
            fail(res, "Сервер не ответил на AUTH TLS."); ch_close(&fs->ch); return FTP_E_NET;
        } else {
            say(o, FTP_SAY_WARN, "Сервер не умеет шифрование (AUTH TLS → %d): логин и пароль пойдут открытым текстом.", code);
        }
    }

    if (fs->control_tls) {
        cmd(fs, "PBSZ", "0", text, sizeof text);
        code = cmd(fs, "PROT", "P", text, sizeof text);
        if (code == 200) fs->data_tls = 1;
        else if (rq->sec == FTP_SEC_TLS) {
            res->code = code; fail(res, "Сервер не принял PROT P (%d): %s", code, text);
            ch_close(&fs->ch); return FTP_E_NEEDTLS;
        } else say(o, FTP_SAY_WARN, "PROT P не принят (%d): данные пойдут без шифрования.", code);
        if (tls_session_save(fs->ch.tls, &fs->sess) == 0) fs->ctl_ver12 = 1;
    }

    fs->tls_flags = tls_flags;
    fs->type = 'I';
    fs->open = 1;
    return FTP_OK;
}

static void login_denied(struct ftp_session *fs, struct ftp_result *res, int code, const char *text) {
    if (!fs->control_tls && (code == 534 || code == 522 || code == 421 || code == 550 || code == 530) &&
        (strstr_n(text, text + strlen(text), "SSL") || strstr_n(text, text + strlen(text), "TLS") ||
         strstr_n(text, text + strlen(text), "ncrypt") || strstr_n(text, text + strlen(text), "ecure")))
        fail(res, "Не пустили: %d %s — сервер требует шифрования (FTPS).", code, text);
    else fail(res, "Не пустили на сервер: %d %s", code, text);
}

/* USER: FTP_OK — уже вошли (230), FTP_NEED_PASS — сервер ждёт пароль (331), иначе ошибка (соединение остаётся открытым) */
static int do_user(struct ftp_session *fs, const char *user, struct ftp_result *res) {
    char text[300];
    int code = cmd(fs, "USER", user, text, sizeof text);
    res->code = code;
    if (code == 230) { fs->logged = 1; return FTP_OK; }
    if (code == 331) return FTP_NEED_PASS;
    if (code == 332) { fail(res, "Сервер просит ACCT — не поддерживается."); return FTP_E_LOGIN; }
    if (code < 0) { fail(res, "Сервер не ответил на логин."); return FTP_E_NET; }
    login_denied(fs, res, code, text);
    return FTP_E_LOGIN;
}

static int do_pass(struct ftp_session *fs, const char *pass, struct ftp_result *res) {
    char text[300];
    int code = cmd(fs, "PASS", pass, text, sizeof text);
    res->code = code;
    if (code == 230 || code == 202) { fs->logged = 1; return FTP_OK; }
    if (code < 0) { fail(res, "Сервер не ответил на логин."); return FTP_E_NET; }
    login_denied(fs, res, code, text);
    return FTP_E_LOGIN;
}

/* подключение + логин одним вызовом (ftp_run и тесты) */
static int do_login(const struct ftp_ops *o, const struct ftp_req *rq, struct ftp_session *fs, struct ftp_result *res, int tls_flags) {
    int r = do_open(o, rq, fs, res, tls_flags);
    if (r != FTP_OK) return r;
    r = do_user(fs, rq->user, res);
    if (r == FTP_NEED_PASS) r = do_pass(fs, rq->pass, res);
    if (r != FTP_OK) { ch_close(&fs->ch); fs->open = 0; return r; }
    return FTP_OK;
}

/* ---------- канал данных (EPSV/иначе PASV) + сама передача; используется и ftp_run, и интерактивными командами ---------- */
static int do_download(struct ftp_session *fs, const char *verb, const char *arg, int is_get, size_t limit, struct ftp_result *res) {
    char text[300]; int code;
    uint16_t dport = 0;
    code = cmd(fs, "EPSV", NULL, text, sizeof text);
    if (code == 229 && parse_epsv(text, &dport) == 0) { /* ok */ }
    else {
        code = cmd(fs, "PASV", NULL, text, sizeof text);
        if (code != 227 || parse_pasv(text, &dport) != 0) {
            res->code = code; fail(res, "Не получилось открыть пассивный канал данных (%d %s).", code, code > 0 ? text : "");
            return FTP_E_CMD;
        }
    }
    struct ftp_chan d; memset(&d, 0, sizeof d); d.ops = fs->ch.ops;
    int derr;
    d.h = fs->ch.ops->dial(fs->ch.ops->ud, fs->ip, dport, 4000, &derr);
    if (!d.h) { fail(res, "Не подключился к каналу данных (порт %d).", (int)dport); return FTP_E_NET; }

    if (is_get && !arg) { fail(res, "Не указан файл."); ch_close(&d); return FTP_E_ARG; }
    if (send_cmd(fs, verb, arg)) { fail(res, "Недопустимый путь."); ch_close(&d); return FTP_E_ARG; }

    if (fs->data_tls) {
        struct tls_io dio = { fs->ch.ops->send, fs->ch.ops->recv, d.h };
        int e;
        d.tls = tls_open_ex(&dio, fs->host, fs->ctl_ver12 ? TLS_F_ONLY12 : fs->tls_flags, fs->ctl_ver12 ? &fs->sess : NULL, &e);
        if (!d.tls) {
            ch_close(&d);
            code = read_reply(fs, text, sizeof text, 4000);
            res->code = code;
            if (code >= 400) fail(res, "%s: %d %s", verb, code, text);
            else fail(res, "TLS на канале данных не удался: %s%s", tls_open_msg(e), fs->ctl_ver12 ? " (сервер мог потребовать общую сессию с каналом управления)" : "");
            return FTP_E_TLS;
        }
        res->resumed = tls_info(d.tls)->resumed;
        report_tls(fs->ch.ops, "Канал данных", d.tls);
    }

    code = read_reply(fs, text, sizeof text, CTL_TO);
    if (code != 150 && code != 125) {
        res->code = code; fail(res, "%s: %d %s", verb, code, code > 0 ? text : "нет ответа");
        ch_close(&d); return FTP_E_CMD;
    }

    size_t cap = 64 * 1024, len = 0;
    uint8_t *db = kmalloc(cap);
    if (!db) { fail(res, "Не хватило памяти."); ch_close(&d); return FTP_E_MEM; }
    int idle = 0;
    for (;;) {
        if (len + 4096 > cap) {
            size_t ncap = cap * 2;
            if (ncap > limit + 65536) { res->truncated = 1; break; }
            uint8_t *nb = kmalloc(ncap);
            if (!nb) { res->truncated = 1; break; }
            memcpy(nb, db, len); kfree(db); db = nb; cap = ncap;
        }
        int n = ch_recv(&d, db + len, cap - len - 1, 3000);
        if (n < 0) break;                                   /* сервер закрыл — конец файла */
        if (n == 0) { if (++idle >= 5) { res->truncated = 1; fail(res, "Передача зависла (нет данных 15 с)."); break; } continue; }
        idle = 0;
        len += (size_t)n;
        if (fs->ch.ops->tick) fs->ch.ops->tick(fs->ch.ops->ud, len);
    }
    ch_close(&d);
    db[len] = 0;

    code = read_reply(fs, text, sizeof text, CTL_TO);       /* 226 Transfer complete */
    if (code >= 400) {
        res->code = code; res->truncated = 1;
        fail(res, "Передача прервана: %d %s", code, text);
        kfree(db); return FTP_E_CMD;
    }
    if (res->truncated && !res->msg[0]) fail(res, "Файл больше лимита %u МиБ — получена только часть.", (unsigned)(limit >> 20));
    res->data = db; res->len = len;
    return FTP_OK;
}

/* ---------- загрузка на сервер (STOR) ---------- */
static int do_upload(struct ftp_session *fs, const char *path, const uint8_t *data, size_t len, struct ftp_result *res) {
    char text[300]; int code;
    uint16_t dport = 0;
    code = cmd(fs, "EPSV", NULL, text, sizeof text);
    if (code == 229 && parse_epsv(text, &dport) == 0) { /* ok */ }
    else {
        code = cmd(fs, "PASV", NULL, text, sizeof text);
        if (code != 227 || parse_pasv(text, &dport) != 0) {
            res->code = code; fail(res, "Не получилось открыть пассивный канал данных (%d %s).", code, code > 0 ? text : "");
            return FTP_E_CMD;
        }
    }
    struct ftp_chan d; memset(&d, 0, sizeof d); d.ops = fs->ch.ops;
    int derr;
    d.h = fs->ch.ops->dial(fs->ch.ops->ud, fs->ip, dport, 4000, &derr);
    if (!d.h) { fail(res, "Не подключился к каналу данных (порт %d).", (int)dport); return FTP_E_NET; }

    if (send_cmd(fs, "STOR", path)) { fail(res, "Недопустимое имя файла."); ch_close(&d); return FTP_E_ARG; }

    if (fs->data_tls) {
        struct tls_io dio = { fs->ch.ops->send, fs->ch.ops->recv, d.h };
        int e;
        d.tls = tls_open_ex(&dio, fs->host, fs->ctl_ver12 ? TLS_F_ONLY12 : fs->tls_flags, fs->ctl_ver12 ? &fs->sess : NULL, &e);
        if (!d.tls) {
            ch_close(&d);
            code = read_reply(fs, text, sizeof text, 4000);
            res->code = code;
            if (code >= 400) fail(res, "STOR: %d %s", code, text);
            else fail(res, "TLS на канале данных не удался: %s", tls_open_msg(e));
            return FTP_E_TLS;
        }
        res->resumed = tls_info(d.tls)->resumed;
        report_tls(fs->ch.ops, "Канал данных", d.tls);
    }

    code = read_reply(fs, text, sizeof text, CTL_TO);
    if (code != 150 && code != 125) {
        res->code = code; fail(res, "STOR: %d %s", code, code > 0 ? text : "нет ответа");
        ch_close(&d); return FTP_E_CMD;
    }

    size_t sent = 0; int ok = 1;
    while (sent < len) {
        size_t chunk = len - sent; if (chunk > 16384) chunk = 16384;
        if (ch_send(&d, data + sent, chunk) != 0) { ok = 0; break; }
        sent += chunk;
        if (fs->ch.ops->tick) fs->ch.ops->tick(fs->ch.ops->ud, sent);
    }
    ch_close(&d);
    if (!ok) { fail(res, "Передача оборвалась после %zu из %zu байт.", sent, len); return FTP_E_NET; }

    code = read_reply(fs, text, sizeof text, CTL_TO);
    if (code >= 400 || (code != 226 && code != 250)) {
        res->code = code; fail(res, "Передача прервана: %d %s", code, code > 0 ? text : "нет ответа");
        return FTP_E_CMD;
    }
    return FTP_OK;
}

/* ---------- разовое подключение (ftp_run): логин, одна команда LIST/RETR, QUIT ---------- */
static int session(const struct ftp_ops *o, const struct ftp_req *rq, struct ftp_result *res, int tls_flags) {
    struct ftp_session fs;
    int r = do_login(o, rq, &fs, res, tls_flags);
    if (r != FTP_OK) return r;
    res->control_tls = fs.control_tls;         /* do_login выставляет это на fs, не на res напрямую */
    res->data_tls = fs.data_tls;

    char text[300];
    cmd(&fs, "TYPE", rq->op == FTP_GET ? "I" : "A", text, sizeof text);

    const char *verb = rq->op == FTP_GET ? "RETR" : "LIST";
    const char *arg = rq->path && *rq->path ? rq->path : NULL;
    int rc = do_download(&fs, verb, arg, rq->op == FTP_GET, rq->op == FTP_GET ? GET_MAX : LIST_MAX, res);

    if (rc == FTP_OK || rc == FTP_E_CMD) send_cmd(&fs, "QUIT", NULL);
    ch_close(&fs.ch);
    return rc;
}

int ftp_run(const struct ftp_ops *o, const struct ftp_req *rq, struct ftp_result *res) {
    memset(res, 0, sizeof *res);
    if (!safe_arg(rq->user) || !safe_arg(rq->pass) || (rq->path && !safe_arg(rq->path)) || !rq->user[0]) { fail(res, "Недопустимые символы в аргументах."); return FTP_E_ARG; }
    /* сначала TLS 1.2: vsftpd/ProFTPD требуют, чтобы канал данных возобновил сессию канала управления, а так это возможно;
       если сервер не знает 1.2 — повторяем всё соединение с предложением 1.3 */
    int r = session(o, rq, res, TLS_F_ONLY12);
    if (r == FTP_E_TLSVER) {
        say(o, FTP_SAY_INFO, "Сервер не принял TLS 1.2 — пробую TLS 1.3.");
        memset(res, 0, sizeof *res);
        r = session(o, rq, res, 0);
        if (r == FTP_E_TLSVER) { fail(res, "Сервер не согласовал ни TLS 1.2, ни 1.3."); r = FTP_E_TLS; }
    }
    return r;
}

/* ================= Живая (интерактивная) сессия ================= */

int ftp_connect(const struct ftp_ops *o, const struct ftp_req *rq, struct ftp_session *fs, struct ftp_result *res) {
    memset(res, 0, sizeof *res);
    if (!safe_arg(rq->user) || !safe_arg(rq->pass) || !rq->user[0]) { fail(res, "Недопустимые символы в аргументах."); return FTP_E_ARG; }
    int r = do_login(o, rq, fs, res, TLS_F_ONLY12);
    if (r == FTP_E_TLSVER) {
        say(o, FTP_SAY_INFO, "Сервер не принял TLS 1.2 — пробую TLS 1.3.");
        memset(res, 0, sizeof *res);
        r = do_login(o, rq, fs, res, 0);
        if (r == FTP_E_TLSVER) { fail(res, "Сервер не согласовал ни TLS 1.2, ни 1.3."); r = FTP_E_TLS; }
    }
    if (r == FTP_OK) { res->control_tls = fs->control_tls; res->data_tls = fs->data_tls; }
    return r;
}

int ftp_list(struct ftp_session *fs, const char *path, struct ftp_result *res) {
    memset(res, 0, sizeof *res);
    if (!fs->open) { fail(res, "Нет соединения."); return FTP_E_NET; }
    if (path && !safe_arg(path)) { fail(res, "Недопустимый путь."); return FTP_E_ARG; }
    char text[300]; cmd(fs, "TYPE", "A", text, sizeof text);
    return do_download(fs, "LIST", path, 0, LIST_MAX, res);
}

int ftp_nlst(struct ftp_session *fs, const char *path, struct ftp_result *res) {
    memset(res, 0, sizeof *res);
    if (!fs->open) { fail(res, "Нет соединения."); return FTP_E_NET; }
    if (path && !safe_arg(path)) { fail(res, "Недопустимый путь."); return FTP_E_ARG; }
    char text[300]; cmd(fs, "TYPE", "A", text, sizeof text);
    return do_download(fs, "NLST", path, 0, LIST_MAX, res);
}

int ftp_get(struct ftp_session *fs, const char *path, struct ftp_result *res) {
    memset(res, 0, sizeof *res);
    if (!fs->open) { fail(res, "Нет соединения."); return FTP_E_NET; }
    if (!path || !*path || !safe_arg(path)) { fail(res, "Не указан файл."); return FTP_E_ARG; }
    char text[300]; char tt[2] = { fs->type ? fs->type : 'I', 0 }; cmd(fs, "TYPE", tt, text, sizeof text);
    return do_download(fs, "RETR", path, 1, GET_MAX, res);
}

int ftp_put(struct ftp_session *fs, const char *path, const uint8_t *data, size_t len, struct ftp_result *res) {
    memset(res, 0, sizeof *res);
    if (!fs->open) { fail(res, "Нет соединения."); return FTP_E_NET; }
    if (!path || !*path || !safe_arg(path)) { fail(res, "Не указано имя файла."); return FTP_E_ARG; }
    char text[300]; char tt[2] = { fs->type ? fs->type : 'I', 0 }; cmd(fs, "TYPE", tt, text, sizeof text);
    return do_upload(fs, path, data, len, res);
}

int ftp_cwd(struct ftp_session *fs, const char *path, struct ftp_result *res) {
    memset(res, 0, sizeof *res);
    if (!fs->open) { fail(res, "Нет соединения."); return FTP_E_NET; }
    if (!path || !safe_arg(path)) { fail(res, "Недопустимый путь."); return FTP_E_ARG; }
    char text[300];
    int code = cmd(fs, "CWD", path, text, sizeof text);
    res->code = code;
    if (code != 250) { fail(res, "%d %s", code, code > 0 ? text : "нет ответа"); return FTP_E_CMD; }
    return FTP_OK;
}

int ftp_pwd(struct ftp_session *fs, char *out, size_t cap, struct ftp_result *res) {
    memset(res, 0, sizeof *res);
    if (!fs->open) { fail(res, "Нет соединения."); return FTP_E_NET; }
    char text[300];
    int code = cmd(fs, "PWD", NULL, text, sizeof text);
    res->code = code;
    if (code != 257) { fail(res, "%d %s", code, code > 0 ? text : "нет ответа"); return FTP_E_CMD; }
    ksnprintf(out, cap, "%s", text);
    return FTP_OK;
}

int ftp_mkdir(struct ftp_session *fs, const char *path, struct ftp_result *res) {
    memset(res, 0, sizeof *res);
    if (!fs->open) { fail(res, "Нет соединения."); return FTP_E_NET; }
    if (!path || !*path || !safe_arg(path)) { fail(res, "Недопустимое имя."); return FTP_E_ARG; }
    char text[300];
    int code = cmd(fs, "MKD", path, text, sizeof text);
    res->code = code;
    if (code != 257 && code != 250) { fail(res, "%d %s", code, code > 0 ? text : "нет ответа"); return FTP_E_CMD; }
    return FTP_OK;
}

int ftp_rm(struct ftp_session *fs, const char *path, struct ftp_result *res) {
    memset(res, 0, sizeof *res);
    if (!fs->open) { fail(res, "Нет соединения."); return FTP_E_NET; }
    if (!path || !*path || !safe_arg(path)) { fail(res, "Недопустимое имя."); return FTP_E_ARG; }
    char text[300];
    int code = cmd(fs, "DELE", path, text, sizeof text);
    res->code = code;
    if (code != 250) { fail(res, "%d %s", code, code > 0 ? text : "нет ответа"); return FTP_E_CMD; }
    return FTP_OK;
}

/* ---- Шаги «как в ftp.exe»: open → (баннер 220) → user → pass ---- */
int ftp_open(const struct ftp_ops *o, const struct ftp_req *rq, struct ftp_session *fs, struct ftp_result *res) {
    memset(res, 0, sizeof *res);
    int r = do_open(o, rq, fs, res, TLS_F_ONLY12);
    if (r == FTP_E_TLSVER) {
        say(o, FTP_SAY_INFO, "Сервер не принял TLS 1.2 — пробую TLS 1.3.");
        memset(res, 0, sizeof *res);
        r = do_open(o, rq, fs, res, 0);
        if (r == FTP_E_TLSVER) { fail(res, "Сервер не согласовал ни TLS 1.2, ни 1.3."); r = FTP_E_TLS; }
    }
    if (r == FTP_OK) { res->control_tls = fs->control_tls; res->data_tls = fs->data_tls; }
    return r;
}

int ftp_user(struct ftp_session *fs, const char *user, struct ftp_result *res) {
    memset(res, 0, sizeof *res);
    if (!fs->open) { fail(res, "Нет соединения."); return FTP_E_NET; }
    if (!user || !*user || !safe_arg(user)) { fail(res, "Недопустимое имя пользователя."); return FTP_E_ARG; }
    fs->logged = 0;
    return do_user(fs, user, res);
}

int ftp_pass(struct ftp_session *fs, const char *pass, struct ftp_result *res) {
    memset(res, 0, sizeof *res);
    if (!fs->open) { fail(res, "Нет соединения."); return FTP_E_NET; }
    if (!safe_arg(pass)) { fail(res, "Недопустимый пароль."); return FTP_E_ARG; }
    return do_pass(fs, pass, res);
}

int ftp_rmdir(struct ftp_session *fs, const char *path, struct ftp_result *res) {
    memset(res, 0, sizeof *res);
    if (!fs->open) { fail(res, "Нет соединения."); return FTP_E_NET; }
    if (!path || !*path || !safe_arg(path)) { fail(res, "Недопустимое имя."); return FTP_E_ARG; }
    char text[300];
    int code = cmd(fs, "RMD", path, text, sizeof text);
    res->code = code;
    if (code != 250) { fail(res, "%d %s", code, code > 0 ? text : "нет ответа"); return FTP_E_CMD; }
    return FTP_OK;
}

int ftp_rename(struct ftp_session *fs, const char *from, const char *to, struct ftp_result *res) {
    memset(res, 0, sizeof *res);
    if (!fs->open) { fail(res, "Нет соединения."); return FTP_E_NET; }
    if (!from || !*from || !to || !*to || !safe_arg(from) || !safe_arg(to)) { fail(res, "Недопустимое имя."); return FTP_E_ARG; }
    char text[300];
    int code = cmd(fs, "RNFR", from, text, sizeof text);
    res->code = code;
    if (code != 350) { fail(res, "%d %s", code, code > 0 ? text : "нет ответа"); return FTP_E_CMD; }
    code = cmd(fs, "RNTO", to, text, sizeof text);
    res->code = code;
    if (code != 250) { fail(res, "%d %s", code, code > 0 ? text : "нет ответа"); return FTP_E_CMD; }
    return FTP_OK;
}

int ftp_cdup(struct ftp_session *fs, struct ftp_result *res) {
    memset(res, 0, sizeof *res);
    if (!fs->open) { fail(res, "Нет соединения."); return FTP_E_NET; }
    char text[300];
    int code = cmd(fs, "CDUP", NULL, text, sizeof text);
    res->code = code;
    if (code != 250 && code != 200) { fail(res, "%d %s", code, code > 0 ? text : "нет ответа"); return FTP_E_CMD; }
    return FTP_OK;
}

/* сырая команда сервера (quote в ftp.exe): отправить и показать ответ */
int ftp_quote(struct ftp_session *fs, const char *line, struct ftp_result *res) {
    memset(res, 0, sizeof *res);
    if (!fs->open) { fail(res, "Нет соединения."); return FTP_E_NET; }
    if (!line || !*line || !safe_arg(line)) { fail(res, "Недопустимая команда."); return FTP_E_ARG; }
    char text[300];
    char verb[16]; size_t i = 0;
    while (line[i] && line[i] != ' ' && i < sizeof verb - 1) { verb[i] = line[i]; i++; }
    verb[i] = 0;
    const char *arg = line[i] == ' ' ? line + i + 1 : NULL;
    int code = cmd(fs, verb, arg && *arg ? arg : NULL, text, sizeof text);
    res->code = code;
    return code < 0 ? FTP_E_NET : FTP_OK;
}

void ftp_close(struct ftp_session *fs) {
    if (!fs->open) return;
    if (send_cmd(fs, "QUIT", NULL) == 0) { char tx[120]; read_reply(fs, tx, sizeof tx, 2000); }   /* «221 Goodbye» как в ftp.exe */
    ch_close(&fs->ch);
    fs->open = 0; fs->logged = 0;
}
