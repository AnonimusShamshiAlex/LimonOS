#include "calc.h"
#include "kstring.h"

static int64_t iabs64(int64_t v) { return v < 0 ? -v : v; }

/* строка вида "-123.456" -> масштабированное целое (CALC_SCALE). Лишние дробные знаки
 * (больше 6) просто отбрасываются — для калькулятора этого достаточно. */
static int64_t parse_scaled(const char *s) {
    int neg = 0; if (*s == '-') { neg = 1; s++; }
    int64_t whole = 0;
    while (*s >= '0' && *s <= '9') whole = whole * 10 + (*s++ - '0');
    int64_t frac = 0, scale = CALC_SCALE;
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9' && scale > 1) { scale /= 10; frac += (int64_t)(*s++ - '0') * scale; }
    }
    int64_t v = whole * CALC_SCALE + frac;
    return neg ? -v : v;
}

/* масштабированное целое -> строка, без лишних нулей справа */
static void format_scaled(int64_t v, char *out, size_t cap) {
    int neg = v < 0; uint64_t av = (uint64_t)iabs64(v);
    uint64_t whole = av / (uint64_t)CALC_SCALE, frac = av % (uint64_t)CALC_SCALE;
    char buf[32]; size_t n = 0;
    if (neg) buf[n++] = '-';
    char wb[20]; int wn = 0;
    if (whole == 0) wb[wn++] = '0';
    while (whole > 0) { wb[wn++] = (char)('0' + whole % 10); whole /= 10; }
    while (wn > 0) buf[n++] = wb[--wn];
    if (frac > 0) {
        buf[n++] = '.';
        char fb[8]; int fn = 0;
        uint64_t div = 100000;
        for (int i = 0; i < 6; i++) { fb[fn++] = (char)('0' + (frac / div) % 10); div /= 10; }
        while (fn > 0 && fb[fn - 1] == '0') fn--;        /* убираем лишние нули справа */
        for (int i = 0; i < fn; i++) buf[n++] = fb[i];
    }
    buf[n] = 0;
    if (n >= cap) { out[0] = '?'; out[1] = 0; return; }   /* не влезает — крайний случай, не должно происходить при разумном вводе */
    memcpy(out, buf, n + 1);
}

void calc_init(struct calc_state *c) { memset(c, 0, sizeof *c); calc_clear(c); }

void calc_clear(struct calc_state *c) {
    c->display[0] = '0'; c->display[1] = 0;
    c->acc = 0; c->pending_op = 0; c->fresh = 1; c->error = 0;
}

void calc_digit(struct calc_state *c, char d) {
    if (c->error) return;
    if (c->fresh) { c->display[0] = d; c->display[1] = 0; c->fresh = 0; return; }
    size_t n = strlen(c->display);
    if (n == 1 && c->display[0] == '0') { c->display[0] = d; c->display[1] = 0; return; }   /* ведущий ноль не копим */
    if (n + 1 >= CALC_DISPLAY_CAP) return;             /* не бесконечно длинное число */
    c->display[n] = d; c->display[n + 1] = 0;
}

void calc_dot(struct calc_state *c) {
    if (c->error) return;
    if (c->fresh) { c->display[0] = '0'; c->display[1] = '.'; c->display[2] = 0; c->fresh = 0; return; }
    if (strchr(c->display, '.')) return;               /* вторая точка не нужна */
    size_t n = strlen(c->display);
    if (n + 1 >= CALC_DISPLAY_CAP) return;
    c->display[n] = '.'; c->display[n + 1] = 0;
}

void calc_backspace(struct calc_state *c) {
    if (c->error) { calc_clear(c); return; }
    if (c->fresh) return;
    size_t n = strlen(c->display);
    if (n <= 1) { c->display[0] = '0'; c->display[1] = 0; c->fresh = 1; return; }
    c->display[n - 1] = 0;
}

void calc_negate(struct calc_state *c) {
    if (c->error) return;
    if (strcmp(c->display, "0") == 0) return;
    if (c->display[0] == '-') memmove(c->display, c->display + 1, strlen(c->display));
    else { size_t n = strlen(c->display); if (n + 1 >= CALC_DISPLAY_CAP) return; memmove(c->display + 1, c->display, n + 1); c->display[0] = '-'; }
}

void calc_percent(struct calc_state *c) {
    if (c->error) return;
    int64_t v = parse_scaled(c->display);
    v = v / 100;                                        /* x% = x/100, как на простых калькуляторах */
    format_scaled(v, c->display, sizeof c->display);
    c->fresh = 1;
}

static int apply(int64_t a, int64_t b, char op, int64_t *out) {
    switch (op) {
    case '+': *out = a + b; return 1;
    case '-': *out = a - b; return 1;
    case '*': *out = (a / 1000) * (b / 1000);            /* a,b масштаб 1e6: (a*b)/1e6, делим заранее, чтобы не переполнить int64 */
              /* точнее пересчитаем без потери точности там, где влезает */
              {
                  /* используем 128-битное умножение, если доступно, иначе — безопасный путь выше */
                  __int128 full = (__int128)a * (__int128)b / CALC_SCALE;
                  if (full > 9000000000000000000LL || full < -9000000000000000000LL) return 0;   /* переполнение */
                  *out = (int64_t)full;
              }
              return 1;
    case '/':
        if (b == 0) return 0;
        {
            __int128 full = (__int128)a * CALC_SCALE / b;
            *out = (int64_t)full;
        }
        return 1;
    default: *out = b; return 1;
    }
}

void calc_op(struct calc_state *c, char op) {
    if (c->error) return;
    if (!c->pending_op) {
        c->acc = parse_scaled(c->display);
    } else if (!c->fresh) {
        /* новое число уже ввели после прошлой операции — применяем предыдущую операцию.
         * Если же оператор нажали второй раз подряд без ввода цифр (c->fresh==1),
         * ничего не считаем — просто меняем операцию, как на обычных калькуляторах. */
        int64_t v = parse_scaled(c->display);
        int64_t r;
        if (!apply(c->acc, v, c->pending_op, &r)) {
            c->error = 1;
            memcpy(c->display, "Ошибка", sizeof "Ошибка");
            c->pending_op = 0; c->fresh = 1;
            return;
        }
        c->acc = r;
        format_scaled(c->acc, c->display, sizeof c->display);
    }
    c->pending_op = op;
    c->fresh = 1;
}

void calc_equals(struct calc_state *c) {
    if (c->error) return;
    int64_t v = parse_scaled(c->display);
    if (!c->pending_op) { c->acc = v; c->fresh = 1; return; }
    int64_t r;
    if (!apply(c->acc, v, c->pending_op, &r)) { c->error = 1; memcpy(c->display, "Ошибка", sizeof "Ошибка"); c->fresh = 1; c->pending_op = 0; return; }
    c->acc = r;
    format_scaled(c->acc, c->display, sizeof c->display);
    c->pending_op = 0;
    c->fresh = 1;
}
