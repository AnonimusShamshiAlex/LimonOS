#include "libc.h"

/* Первая настоящая программа LimonOS: отдельный ELF-файл, собранный gcc+ld,
 * загружаемый ядром (kernel/elf.c + kernel/exec.c) и работающий в ring3 через свою libc.
 * Вывод проверяется побайтно (kernel/exec.h: EXEC_HELLO_EXPECT). */
int main(int argc, char **argv) {
    puts("Привет из настоящего ELF!");
    printf("argc=%d", argc);
    for (int i = 0; i < argc; i++) printf(" argv[%d]=%s", i, argv[i]);
    putchar('\n');
    printf("printf: [%4d|%-4d|%05d] %d %x %u %c\n", 42, 42, 42, -7, 255, 65535u, 'A');

    int *a = malloc(1000 * sizeof *a);
    if (!a) { puts("malloc: ОШИБКА"); return 2; }
    long sum = 0;
    for (int i = 0; i < 1000; i++) a[i] = i;
    for (int i = 0; i < 1000; i++) sum += a[i];
    free(a);
    printf("malloc: 1000 ints ok, sum=%ld\n", sum);

    printf("strings: strlen=%d eq=%d lt=%d\n", (int)strlen("limon"), strcmp("a", "a") == 0, strcmp("a", "b") < 0);
    return 0;
}
