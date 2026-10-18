#ifndef LIMON_SHELL_H
#define LIMON_SHELL_H
#include <stdint.h>
#include "term.h"
#include "keyboard.h"

#define SH_LINE_MAX  72
#define SH_HIST      16
#define SH_HIST_BYTES (SH_LINE_MAX * 4 + 1)

struct shell {
    struct term *t;
    uint32_t line[SH_LINE_MAX];
    int len, pos;
    char hist[SH_HIST][SH_HIST_BYTES];
    int hist_count, hist_idx;
};

void shell_init(struct shell *s, struct term *t);
void shell_key(struct shell *s, const struct key_event *ev);

#endif
