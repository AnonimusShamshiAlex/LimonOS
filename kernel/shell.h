#ifndef LIMON_SHELL_H
#define LIMON_SHELL_H
#include <stdint.h>
#include "term.h"
#include "keyboard.h"

#define SH_LINE_MAX  1024   /* хватит на длинный адрес, запрос к ai или ключ API — строка при нужде прокручивается */
#define SH_HIST      16
#define SH_HIST_BYTES (SH_LINE_MAX * 4 + 1)

struct shell {
    struct term *t;
    uint32_t line[SH_LINE_MAX];
    int len, pos;
    int scroll;    /* индекс первого видимого символа строки (для прокрутки длинных команд) */
    char hist[SH_HIST][SH_HIST_BYTES];
    int hist_count, hist_idx;
    int cwd;       /* текущая папка в Memoria (vfs.h), по умолчанию /Downloads */
};

void shell_init(struct shell *s, struct term *t);
void shell_key(struct shell *s, const struct key_event *ev);

#endif
