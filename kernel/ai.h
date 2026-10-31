#ifndef LIMON_AI_H
#define LIMON_AI_H
/* Spark — AI-помощник прямо в терминале LimonOS. Говорит с Anthropic API (Claude) по https.
   Ключ API хранится только в памяти (пропадёт при выключении) — его задаёт сам человек
   командой `aikey`, в коде LimonOS ключа нет и быть не должно. */

enum ai_provider {
    AI_PROVIDER_NONE = 0,
    AI_PROVIDER_ANTHROPIC,   /* ключ вида sk-ant-...        : api.anthropic.com, формат Messages API */
    AI_PROVIDER_OPENROUTER,  /* ключ вида sk-or-v1-...       : openrouter.ai, формат OpenAI chat/completions */
};

/* Поставщик определяется САМ по виду ключа (sk-ant- / sk-or-), чтобы не путать человека —
   не нужно отдельно указывать, какой это сервис. */
void ai_set_key(const char *key);         /* NULL или "" — убрать ключ */
int  ai_has_key(void);
enum ai_provider ai_get_provider(void);
const char *ai_provider_name(void);
int  ai_set_proxy(const char *base_url, enum ai_provider provider);   /* "" — выключить; 0 — успех; -1 — нужен адрес http://... */
const char *ai_proxy(void);
void ai_set_model(const char *model);     /* своя модель вместо умолчания для текущего поставщика */
const char *ai_model(void);

/* Показывает, что РЕАЛЬНО уйдёт на сервер (адрес, заголовки, тело запроса) — ничего не отправляет.
   Нужно, чтобы своими глазами увидеть, не портится ли ключ где-то по пути, а не гадать по коду. */
void ai_dump_request(const char *question, void (*on_line)(const char *text, void *ctx), void *ctx);

/* Печатает ответ прямо в терминал по ходу запроса (через callback), чтобы видно было, что не зависло.
   Возвращает 0 при успехе; при ошибке сообщение уже выведено через on_line("Ошибка: ..."). */
int ai_ask(const char *question, void (*on_line)(const char *text, void *ctx), void *ctx);

#endif
