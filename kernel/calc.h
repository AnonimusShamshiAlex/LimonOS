#ifndef LIMON_CALC_H
#define LIMON_CALC_H
#include <stdint.h>

/* Zest-калькулятор: обычный 4-функционный калькулятор (не abacus — тот считает выражения
 * целиком по правилам математики; этот работает как настоящая кнопочная машинка: слева
 * направо, одна операция за раз). Ядро собрано без FPU/SSE (-mno-sse -mno-80387), поэтому
 * дробные числа — это фиксированная точка на целых int64, без float/double. */
#define CALC_SCALE 1000000LL   /* 6 знаков после запятой */
#define CALC_DISPLAY_CAP 24

struct calc_state {
    char display[CALC_DISPLAY_CAP];   /* то, что видно на экране */
    int64_t acc;                       /* накопленное значение (масштаб CALC_SCALE) */
    char pending_op;                   /* '+','-','*','/' или 0 — операция ждёт второе число */
    int fresh;                         /* 1 — следующая цифра начинает новое число */
    int error;                         /* 1 — деление на 0 и т.п.; ждём очистки */
};

void calc_init(struct calc_state *c);
void calc_clear(struct calc_state *c);
void calc_digit(struct calc_state *c, char d);     /* '0'..'9' */
void calc_dot(struct calc_state *c);
void calc_backspace(struct calc_state *c);
void calc_negate(struct calc_state *c);
void calc_percent(struct calc_state *c);
void calc_op(struct calc_state *c, char op);        /* '+' '-' '*' '/' */
void calc_equals(struct calc_state *c);

#endif
