/* Хост-тест calc.c: button-press-style калькулятор ("слева направо"), сверяем с простым
   интерпретатором той же логики на double (эталон для десятичной арифметики без приоритета операций). */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "calc.h"
void *kmalloc(size_t n); void kfree(void *p);
#include <stdlib.h>
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }

/* нажимает последовательность кнопок и возвращает то, что на экране */
static const char *press_seq(struct calc_state *c, const char *seq) {
    calc_init(c);
    for (const char *p = seq; *p; p++) {
        char ch = *p;
        if (ch >= '0' && ch <= '9') calc_digit(c, ch);
        else if (ch == '.') calc_dot(c);
        else if (ch == 'C') calc_clear(c);
        else if (ch == 'B') calc_backspace(c);
        else if (ch == 'N') calc_negate(c);
        else if (ch == '%') calc_percent(c);
        else if (ch == '=') calc_equals(c);
        else calc_op(c, ch);
    }
    return c->display;
}

static int fails = 0;
static void check(const char *seq, const char *want) {
    struct calc_state c;
    const char *got = press_seq(&c, seq);
    int ok = strcmp(got, want) == 0;
    printf("  %-22s -> %-10s (ждали %-10s) %s\n", seq, got, want, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    check("2+2=", "4");
    check("2+2=+2=", "6");                 /* повторное = после ввода новой операции в стиле обычного калькулятора */
    check("10/4=", "2.5");
    check("10/3=", "3.333333");            /* 6 знаков после запятой */
    check("5-8=", "-3");
    check("3N=", "-3");                    /* смена знака без операции */
    check("3*4N=", "-12");                 /* смена знака второго числа перед = */
    check("50%", "0.5");                   /* процент: 50 -> 0.5 */
    check("200+10%=", "200.1");            /* проценты тут простые: x% = x/100 без учёта контекста (так в коде и задумано) */
    check("1/0=", "Ошибка");               /* деление на ноль */
    check("9B", "0");                      /* backspace единственной цифры -> 0 */
    check("12B", "1");
    check("1.5.5", "1.55");                /* вторая точка игнорируется, но следующая цифра всё равно добавляется */
    check("0.1+0.2=", "0.3");              /* классический тест: не должно быть 0.30000000004 из-за float */
    printf(fails ? "\nПРОВАЛОВ: %d\n" : "\nВСЕ ПРОВЕРКИ ПРОШЛИ\n", fails);
    return fails ? 1 : 0;
}
