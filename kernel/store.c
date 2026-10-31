#include "store.h"
#include "kstring.h"
#include "mem.h"

static struct store_file files[STORE_MAX];

static struct store_file *find(const char *name) {
    for (int i = 0; i < STORE_MAX; i++) if (files[i].used && strcmp(files[i].name, name) == 0) return &files[i];
    return NULL;
}

/* Имя файла: не пусто, без управляющих символов, без «/» и «\\» (защита от путей вроде ../x). */
int store_name_ok(const char *name) {
    if (!name || !name[0]) return 0;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\') return 0;
    }
    return 1;
}

int store_put(const char *name, uint8_t *data, size_t len) {
    if (!store_name_ok(name)) return -2;
    struct store_file *f = find(name);
    if (!f) for (int i = 0; i < STORE_MAX; i++) if (!files[i].used) { f = &files[i]; break; }
    if (!f) return -1;
    if (f->used && f->data) kfree(f->data);
    size_t n = strlen(name); if (n > 47) n = 47;
    while (n > 0 && ((unsigned char)name[n] & 0xC0) == 0x80) n--;      /* не режем UTF-8 символ пополам */
    memcpy(f->name, name, n); f->name[n] = 0;
    f->data = data; f->len = len; f->used = 1;
    return 0;
}

const struct store_file *store_get(const char *name) { return find(name); }

const struct store_file *store_at(int i) {
    int k = 0;
    for (int j = 0; j < STORE_MAX; j++) if (files[j].used) { if (k == i) return &files[j]; k++; }
    return NULL;
}

int store_drop(const char *name) {
    struct store_file *f = find(name);
    if (!f) return -1;
    if (f->data) kfree(f->data);
    memset(f, 0, sizeof *f);
    return 0;
}

size_t store_total(void) {
    size_t s = 0;
    for (int i = 0; i < STORE_MAX; i++) if (files[i].used) s += files[i].len;
    return s;
}
