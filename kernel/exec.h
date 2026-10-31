#ifndef LIMON_EXEC_H
#define LIMON_EXEC_H
#include <stdint.h>
#include <stddef.h>
#include "elf.h"

/*
 * Фаза 5: загрузка ELF-программы в адресное пространство процесса и построение
 * начального стека (argc/argv/envp/auxv по System V x86-64 ABI).
 *
 * Чистая логика: память процесса достаётся через exec_ops, поэтому загрузчик
 * проверяется на хосте (tests/exec_host.c) — в том числе реальным запуском
 * загруженного образа.
 */
#define EXEC_IMAGE_LO     0x0000000000400000ULL   /* образ программы должен лежать в [LO, HI) */
#define EXEC_IMAGE_HI     0x0000000010000000ULL   /* выше — куча (brk), см. syscall.h */
#define EXEC_MAX_PAGES    64                       /* образ не больше 256 КиБ */
#define EXEC_STACK_PAGES  8                        /* стек программы 32 КиБ, растёт вниз от USER_STACK_TOP */
#define EXEC_MAX_ARGS     32

enum exec_err {
    EXEC_OK = 0,
    EXEC_ERR_RANGE,      /* сегмент вне [EXEC_IMAGE_LO, EXEC_IMAGE_HI) */
    EXEC_ERR_TOO_BIG,    /* образ не помещается в EXEC_MAX_PAGES */
    EXEC_ERR_NOMEM,      /* ядро не дало страницу */
    EXEC_ERR_ARGS,       /* аргументы не помещаются в страницу стека */
};

struct exec_ops {
    void *ctx;
    /* Отображает ОДНУ новую ОБНУЛЁННУЮ страницу процесса по va (кратен 4096).
     * writable: нужна ли запись. Возвращает указатель, через который ЯДРО может писать в эту страницу;
     * NULL — не удалось. */
    uint8_t *(*page)(void *ctx, uint64_t va, int writable);
};

/* Грузит все PT_LOAD сегменты: страницы создаются один раз (если два сегмента делят страницу — она
 * общая, и записываемая, если записываем хотя бы один), содержимое копируется из file, хвост (.bss)
 * остаётся нулями. info — результат elf64_parse(file, len). */
enum exec_err exec_load(const struct elf64_info *info, const uint8_t *file, size_t len, const struct exec_ops *ops);

/* Строит начальный стек в верхней странице стека. top_page — указатель на память страницы,
 * соответствующей адресам [top_va - 4096, top_va). На выходе *rsp — значение rsp при входе в _start:
 * [rsp]=argc, затем argv[0..argc-1], NULL, envp (пусто) NULL, auxv AT_NULL; rsp кратен 16. */
enum exec_err exec_build_stack(uint8_t *top_page, uint64_t top_va, int argc, const char *const *argv, uint64_t *rsp);

const char *exec_err_str(enum exec_err e);

/* Что должна напечатать программа userland/hello.c при запуске как `hello.elf LimonOS` */
#define EXEC_HELLO_EXPECT \
    "Привет из настоящего ELF!\n" \
    "argc=2 argv[0]=hello.elf argv[1]=LimonOS\n" \
    "printf: [  42|42  |00042] -7 ff 65535 A\n" \
    "malloc: 1000 ints ok, sum=499500\n" \
    "strings: strlen=5 eq=1 lt=1\n"

#endif
