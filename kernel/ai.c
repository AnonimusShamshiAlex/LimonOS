#include "ai.h"
#include "http.h"
#include "tls.h"
#include "kstring.h"
#include "mem.h"
#include "format.h"
#include "net.h"

#define KEY_MAX 160
#define MODEL_MAX 64

static char g_key[KEY_MAX];
static char g_model[MODEL_MAX];
static int  g_model_set;        /* 1 — человек сам задал модель, не подставлять умолчание поставщика */
static enum ai_provider g_provider = AI_PROVIDER_NONE;
static char g_proxy[128];       /* адрес прокси вида http://192.168.0.5:8787 — тогда ключ хранит прокси, а не LimonOS */
static char g_url[200];

static enum ai_provider detect_provider(const char *key) {
    if (strncmp(key, "sk-ant-", 7) == 0) return AI_PROVIDER_ANTHROPIC;
    if (strncmp(key, "sk-or-", 6) == 0) return AI_PROVIDER_OPENROUTER;
    return AI_PROVIDER_NONE;
}

void ai_set_key(const char *key) {
    if (!key || !key[0]) { secure_zero(g_key, sizeof g_key); g_provider = AI_PROVIDER_NONE; return; }
    secure_zero(g_key, sizeof g_key);
    ksnprintf(g_key, sizeof g_key, "%s", key);
    g_provider = detect_provider(g_key);
}
int ai_has_key(void) { return (g_key[0] != 0 && g_provider != AI_PROVIDER_NONE) || g_proxy[0] != 0; }

/* Режим прокси: LimonOS шлёт вопрос обычным HTTP на твой основной ПК (tools/ai_proxy.py), а тот уже говорит с
   поставщиком по HTTPS со своим ключом. Старому процессору не нужно шифрование, а ключ вообще не попадает в LimonOS. */
int ai_set_proxy(const char *base, enum ai_provider provider) {
    if (!base || !base[0]) { g_proxy[0] = 0; return 0; }
    if (strncmp(base, "http://", 7) != 0 || strlen(base) >= sizeof g_proxy - 1) return -1;
    ksnprintf(g_proxy, sizeof g_proxy, "%s", base);
    size_t n = strlen(g_proxy);
    while (n > 7 && g_proxy[n - 1] == '/') g_proxy[--n] = 0;
    if (provider != AI_PROVIDER_NONE && !g_key[0]) g_provider = provider;
    return 0;
}
const char *ai_proxy(void) { return g_proxy[0] ? g_proxy : NULL; }
enum ai_provider ai_get_provider(void) { return g_provider; }
const char *ai_provider_name(void) {
    switch (g_provider) {
    case AI_PROVIDER_ANTHROPIC: return "Anthropic";
    case AI_PROVIDER_OPENROUTER: return "OpenRouter";
    default: return "неизвестен";
    }
}
void ai_set_model(const char *model) { if (model && model[0]) { ksnprintf(g_model, sizeof g_model, "%s", model); g_model_set = 1; } }

const char *ai_model(void) {
    if (g_model_set) return g_model;
    if (g_provider == AI_PROVIDER_OPENROUTER) return "openrouter/auto";   /* сам выберет подходящую модель */
    return "claude-haiku-5-5";                                           /* по умолчанию для Anthropic: быстрая и дешёвая */
}

/* Экранирует текст для строки в JSON: " \ и управляющие символы. */
static size_t json_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 6 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (c < 0x20) { /* прочие управляющие — пропускаем */ }
        else out[o++] = (char)c;
    }
    out[o] = 0;
    return o;
}

/* Ищет "ключ": "значение в JSON (терпимо к пробелам после двоеточия); пропускает совпадения,
   где после найденного слова не следует ':' — то есть это было чьё-то значение, а не сам ключ.
   Возвращает указатель сразу после открывающей кавычки значения, или NULL. */
static const char *json_find_str(const char *p, const char *end, const char *key) {
    char pat[40];
    ksnprintf(pat, sizeof pat, "\"%s\"", key);
    size_t patlen = strlen(pat);
    const char *from = p;
    while (from < end) {
        const char *m = strstr_n(from, end, pat);
        if (!m) return NULL;
        const char *v = m + patlen;
        while (v < end && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
        if (v < end && *v == ':') {
            v++;
            while (v < end && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
            if (v < end && *v == '"') return v + 1;
        }
        from = m + 1;
    }
    return NULL;
}

/* Распаковывает экранирование JSON-строки на лету, построчно (\n — конец строки для колбэка). */
static void emit_decoded(const char *s, const char *end, void (*on_line)(const char *, void *), void *ctx) {
    char line[400];
    size_t o = 0;
    while (s < end) {
        char c = *s++;
        if (c == '\\' && s < end) {
            char e = *s++;
            if (e == 'n') { line[o] = 0; on_line(line, ctx); o = 0; continue; }
            else if (e == 't') c = '\t';
            else if (e == 'r') { continue; }
            else if (e == '"' || e == '\\' || e == '/') {
                if (o + 1 < sizeof line) line[o++] = e;
                else { line[o] = 0; on_line(line, ctx); o = 0; line[o++] = e; }
                continue;
            }
            else if (e == 'u' && s + 4 <= end) {
                unsigned v = 0;
                for (int i = 0; i < 4; i++) { char h = s[i]; unsigned d = (h >= '0' && h <= '9') ? (unsigned)(h - '0') : (h >= 'a' && h <= 'f') ? (unsigned)(h - 'a' + 10) : (h >= 'A' && h <= 'F') ? (unsigned)(h - 'A' + 10) : 0; v = (v << 4) | d; }
                s += 4;
                char tmp[4]; int n = utf8_encode(v, tmp);
                for (int i = 0; i < n; i++) { if (o + 1 < sizeof line) line[o++] = tmp[i]; else { line[o] = 0; on_line(line, ctx); o = 0; line[o++] = tmp[i]; } }
                continue;
            } else c = e;
        } else if (c == '"') break;
        if (o + 1 < sizeof line) line[o++] = c;
        else { line[o] = 0; on_line(line, ctx); o = 0; line[o++] = c; }
    }
    if (o) { line[o] = 0; on_line(line, ctx); }
}

#ifndef AI_URL_ANTHROPIC
#define AI_URL_ANTHROPIC "https://api.anthropic.com/v1/messages"
#endif
#ifndef AI_URL_OPENROUTER
#define AI_URL_OPENROUTER "https://openrouter.ai/api/v1/chat/completions"
#endif

/* Общая сборка запроса для ai_ask() и ai_dump_request() — чтобы «что показываем» и
   «что реально отправляем» гарантированно были ОДНИМ И ТЕМ ЖЕ кодом, не двумя копиями. */
static int build_request(const char *question, const char **url, char *body, size_t body_cap, char *hdr, size_t hdr_cap) {
    char qesc[2048];
    json_escape(question, qesc, sizeof qesc);
    int anth = g_provider == AI_PROVIDER_ANTHROPIC;
    int bl = ksnprintf(body, body_cap, "{\"model\":\"%s\",\"max_tokens\":1024,\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}", ai_model(), qesc);
    if (g_proxy[0]) { ksnprintf(g_url, sizeof g_url, "%s%s", g_proxy, anth ? "/v1/messages" : "/api/v1/chat/completions"); *url = g_url; }
    else *url = anth ? AI_URL_ANTHROPIC : AI_URL_OPENROUTER;
    hdr[0] = 0;
    if (anth) {
        if (g_key[0]) ksnprintf(hdr, hdr_cap, "x-api-key: %s\r\nanthropic-version: 2023-06-01\r\n", g_key);
        else ksnprintf(hdr, hdr_cap, "anthropic-version: 2023-06-01\r\n");
    } else if (g_key[0]) {
        ksnprintf(hdr, hdr_cap, "Authorization: Bearer %s\r\nHTTP-Referer: https://limonos.local\r\nX-Title: LimonOS\r\n", g_key);
    }
    return bl;
}

void ai_dump_request(const char *question, void (*on_line)(const char *, void *), void *ctx) {
    if (!ai_has_key()) { on_line("Ошибка: сначала задай ключ — aikey sk-ant-... или aikey sk-or-v1-...  (или прокси: aikey proxy http://адрес:порт)", ctx); return; }
    char body[4096], hdr[256]; const char *url;
    int bl = build_request(question, &url, body, sizeof body, hdr, sizeof hdr);
    char line[420];
    ksnprintf(line, sizeof line, "POST %s", url); on_line(line, ctx);
    {   /* ключ показываем только по краям: экран могут видеть или сфотографировать; длина и края позволяют сверить */
        size_t kl = strlen(g_key);
        if (kl) {
            char masked[40];
            if (kl > 16) ksnprintf(masked, sizeof masked, "%.10s...%s", g_key, g_key + kl - 4);
            else ksnprintf(masked, sizeof masked, "(короткий ключ скрыт)");
            ksnprintf(line, sizeof line, "(ключ в памяти: %u символов, начало и конец: %s)", (unsigned)kl, masked); on_line(line, ctx);
        } else on_line("(ключа в LimonOS нет — работает прокси, ключ хранит он)", ctx);
    }
    on_line("--- заголовки ---", ctx);
    /* печатаем hdr построчно, разбивая по \r\n */
    const char *p = hdr;
    while (*p) {
        const char *e = strstr_n(p, p + strlen(p), "\r\n");
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n) { char l2[200]; size_t k = n < sizeof l2 - 1 ? n : sizeof l2 - 1; memcpy(l2, p, k); l2[k] = 0; on_line(l2, ctx); }
        if (!e) break;
        p = e + 2;
    }
    ksnprintf(line, sizeof line, "--- тело (%d байт) ---", bl); on_line(line, ctx);
    on_line(body, ctx);
}

static void report_http_error(int rc, const struct http_resp *r, void (*on_line)(const char *, void *), void *ctx) {
    char msg[96];
    if (rc == HTTP_E_TLS) ksnprintf(msg, sizeof msg, "Ошибка TLS: %s", tls_strerror(r->tls_err));
    else ksnprintf(msg, sizeof msg, "Ошибка сети: %s", http_strerror(rc));
    on_line(msg, ctx);
}

int ai_ask(const char *question, void (*on_line)(const char *, void *), void *ctx) {
    if (!ai_has_key()) { on_line("Ошибка: сначала задай ключ — aikey sk-ant-... (Anthropic) или aikey sk-or-v1-... (OpenRouter)", ctx); return -1; }
    char *body = kmalloc(4096);
    char *hdr = kmalloc(256);
    if (!body || !hdr) { if (body) kfree(body); if (hdr) kfree(hdr); on_line("Ошибка: не хватило памяти", ctx); return -1; }
    const char *url;
    int bl = build_request(question, &url, body, 4096, hdr, 256);

    struct http_opts o; memset(&o, 0, sizeof o);
    o.method = "POST";
    o.body = body; o.body_len = (size_t)bl;
    o.content_type = "application/json";
    o.extra_headers = hdr;
    o.max_bytes = 1u << 20;
    o.timeout_ms = 30000;                                /* модель думает — ждём дольше, чем обычную страницу */

    struct http_resp r;
    int rc = http_fetch(url, &o, &r);
    kfree(body); kfree(hdr);
    if (rc != HTTP_OK) { report_http_error(rc, &r, on_line, ctx); http_free(&r); return -1; }

    const char *p = (const char *)r.data, *end = p + r.len;
    if (r.status != 200) {
        char msg[96];
        ksnprintf(msg, sizeof msg, "Сервер (%s) ответил %d:", ai_provider_name(), r.status);
        on_line(msg, ctx);
        const char *em = json_find_str(p, end, "message");
        if (em) emit_decoded(em, end, on_line, ctx);
        else { size_t n = r.len < 200 ? r.len : 200; char tmp[201]; memcpy(tmp, p, n); tmp[n] = 0; on_line(tmp, ctx); }
        http_free(&r);
        return -1;
    }

    /* Anthropic: content[].text — OpenRouter/OpenAI: choices[].message.content.
       У обоих это просто "ключ": "строка" где-то в ответе — один и тот же разбор годится на оба формата. */
    const char *key = g_provider == AI_PROVIDER_ANTHROPIC ? "text" : "content";
    const char *t = json_find_str(p, end, key);
    if (!t && g_provider == AI_PROVIDER_OPENROUTER) t = json_find_str(p, end, "text");   /* некоторые модели через OpenRouter тоже шлют text */
    if (!t) { on_line("(пустой ответ — модель могла отказаться, быть недоступна у поставщика, или вернуть только другие блоки)", ctx); http_free(&r); return -1; }
    emit_decoded(t, end, on_line, ctx);
    http_free(&r);
    return 0;
}
