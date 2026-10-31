#ifndef LIMON_FTP_H
#define LIMON_FTP_H
#include <stdint.h>
#include <stddef.h>
#include "tls.h"

/* Настоящий клиент FTP/FTPS (RFC 959, RFC 4217, RFC 2428): USER/PASS, AUTH TLS + PBSZ/PROT P, EPSV/PASV, LIST, RETR.
   Модуль ничего не знает про терминал и про сетевой стек ядра — всё приходит через struct ftp_ops, поэтому
   его можно прогнать на хосте против настоящего сервера (tests/run_ftp_test.sh). */

struct ftp_ops {
    void *ud;
    /* открыть TCP-соединение; NULL при ошибке, в *err: -2 не отвечает, -3 порт закрыт, прочее — не удалось */
    void *(*dial)(void *ud, uint32_t ip, uint16_t port, int timeout_ms, int *err);
    int   (*send)(void *h, const void *data, size_t len);                  /* 0 — успех */
    int   (*recv)(void *h, void *buf, size_t max, int timeout_ms);        /* >0 байт, 0 таймаут, <0 закрыто */
    void  (*close)(void *h);
    void  (*say)(void *ud, int kind, const char *text);                    /* протокол «как есть» + пояснения */
    void  (*tick)(void *ud, size_t bytes);                                 /* можно NULL: вызывается по ходу передачи */
};

enum { FTP_SAY_INFO, FTP_SAY_CMD, FTP_SAY_REPLY, FTP_SAY_WARN, FTP_SAY_OK };
enum { FTP_LIST, FTP_GET };
enum { FTP_SEC_AUTO,    /* пробуем AUTH TLS; если сервер не умеет — открытый FTP с предупреждением */
       FTP_SEC_TLS,     /* только с шифрованием */
       FTP_SEC_PLAIN }; /* никогда не шифровать */

struct ftp_req {
    const char *host;          /* для SNI */
    uint32_t ip; uint16_t port;
    const char *user, *pass;
    int op;
    const char *path;          /* LIST: папка (можно NULL); GET: файл */
    int sec;
    int implicit;              /* неявный FTPS (порт 990): TLS сразу после соединения */
};

enum { FTP_OK = 0, FTP_E_NET = -1, FTP_E_TLS = -2, FTP_E_LOGIN = -3, FTP_E_CMD = -4, FTP_E_MEM = -5,
       FTP_E_NEEDTLS = -6, FTP_E_TLSVER = -7 /* внутренний: повторить с TLS 1.3 */, FTP_E_ARG = -8,
       FTP_NEED_PASS = 1 /* не ошибка: сервер ждёт пароль */ };

struct ftp_result {
    uint8_t *data;             /* kmalloc; освободить kfree */
    size_t len;
    int code;                  /* последний код ответа сервера при ошибке */
    char msg[256];             /* человекочитаемая причина ошибки */
    int control_tls, data_tls, resumed;
    int truncated;
};

int ftp_run(const struct ftp_ops *ops, const struct ftp_req *req, struct ftp_result *res);

/* ---- Живая (интерактивная) сессия: как настоящий ftp.exe после логина — канал управления
   остаётся открытым между командами, не «подключился-скачал-отключился» как ftp_run выше. ---- */

struct ftp_chan { const struct ftp_ops *ops; void *h; struct tls *tls; };

struct ftp_session {
    struct ftp_chan ch;
    char buf[2048]; size_t len;
    const char *host;                 /* для SNI канала данных */
    uint32_t ip;
    int tls_flags;                    /* версия TLS, согласованная на канале управления */
    struct tls_session sess;          /* для возобновления TLS-сессии на канале данных (vsftpd и т.п.) */
    int ctl_ver12;
    int control_tls, data_tls;
    int open;                         /* канал управления открыт */
    int logged;                       /* вошли (230) */
    char type;                        /* 'I' бинарный, 'A' текстовый — для get/put */
};

/* Подключается и логинится (host/ip/port/user/pass/sec/implicit из req; op/path не используются).
   При успехе соединение остаётся открытым — закрыть через ftp_close(). */
int  ftp_connect(const struct ftp_ops *ops, const struct ftp_req *req, struct ftp_session *fs, struct ftp_result *res);
/* Шаги «как в ftp.exe»: ftp_open (подключиться, показать баннер, БЕЗ логина) → ftp_user → при FTP_NEED_PASS ftp_pass.
   Пока вы сами не ввели логин/пароль, клиент ничего на сервер не шлёт. */
int  ftp_open(const struct ftp_ops *ops, const struct ftp_req *req, struct ftp_session *fs, struct ftp_result *res);
int  ftp_user(struct ftp_session *fs, const char *user, struct ftp_result *res);
int  ftp_pass(struct ftp_session *fs, const char *pass, struct ftp_result *res);
int  ftp_nlst(struct ftp_session *fs, const char *path, struct ftp_result *res);   /* «ls»: только имена */
int  ftp_rmdir(struct ftp_session *fs, const char *path, struct ftp_result *res);
int  ftp_rename(struct ftp_session *fs, const char *from, const char *to, struct ftp_result *res);
int  ftp_cdup(struct ftp_session *fs, struct ftp_result *res);
int  ftp_quote(struct ftp_session *fs, const char *line, struct ftp_result *res);
int  ftp_cwd(struct ftp_session *fs, const char *path, struct ftp_result *res);
int  ftp_pwd(struct ftp_session *fs, char *out, size_t cap, struct ftp_result *res);
int  ftp_list(struct ftp_session *fs, const char *path, struct ftp_result *res);
int  ftp_get(struct ftp_session *fs, const char *path, struct ftp_result *res);
int  ftp_put(struct ftp_session *fs, const char *path, const uint8_t *data, size_t len, struct ftp_result *res);
int  ftp_mkdir(struct ftp_session *fs, const char *path, struct ftp_result *res);
int  ftp_rm(struct ftp_session *fs, const char *path, struct ftp_result *res);
void ftp_close(struct ftp_session *fs);   /* QUIT + закрыть; безопасно звать на уже закрытой сессии */

#endif
