#include "fat32.h"
#include "mem.h"
#include "kstring.h"

static const struct blkdev *dev;
static uint64_t part_lba, fat_lba, data_lba;
static uint32_t spc, fat_sz, root_cl, ncl;
static uint8_t *cl_buf;                 /* буфер одного кластера */
static uint8_t fat_sec[512];
static uint64_t fat_sec_lba = ~0ULL;
static int mounted;

static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

int fat_mounted(void) { return mounted; }

static int try_bpb(uint64_t lba) {
    uint8_t s[512];
    if (dev->read(dev->ctx, lba, 1, s)) return -3;
    if (s[510] != 0x55 || s[511] != 0xAA) return -1;
    if (rd16(s + 11) != 512 || !s[13] || (s[13] & (s[13] - 1))) return -1;      /* только 512 байт/сектор, кластер — степень двойки */
    if (rd16(s + 17) != 0 || rd16(s + 22) != 0 || !rd32(s + 36)) return -1;     /* у FAT32 нет фиксированного корня и FAT16-размера */
    uint32_t reserved = rd16(s + 14), nfats = s[16], tot = rd32(s + 32);
    if (!reserved || !nfats || !tot) return -1;
    spc = s[13]; fat_sz = rd32(s + 36); root_cl = rd32(s + 44);
    fat_lba = lba + reserved;
    data_lba = fat_lba + (uint64_t)nfats * fat_sz;
    if (tot <= reserved + (uint64_t)nfats * fat_sz) return -1;
    ncl = (tot - reserved - nfats * fat_sz) / spc;
    if (ncl < 65525 || root_cl < 2 || root_cl >= ncl + 2) return -1;            /* меньше 65525 кластеров — это FAT12/16 */
    part_lba = lba;
    return 0;
}

int fat_mount(const struct blkdev *d) {
    mounted = 0; dev = d; fat_sec_lba = ~0ULL;
    if (cl_buf) { kfree(cl_buf); cl_buf = NULL; }
    if (!d) return -1;
    int r = try_bpb(0);
    if (r != 0) {
        uint8_t mbr[512];
        if (d->read(d->ctx, 0, 1, mbr)) return -3;
        if (mbr[510] != 0x55 || mbr[511] != 0xAA) return -1;
        r = -1;
        for (int i = 0; i < 4 && r != 0; i++) {
            const uint8_t *e = mbr + 446 + i * 16;
            if (e[4] == 0x0B || e[4] == 0x0C) r = try_bpb(rd32(e + 8));
        }
    }
    if (r != 0) return r;
    cl_buf = kmalloc((size_t)spc * 512);
    if (!cl_buf) return -2;
    mounted = 1;
    return 0;
}

static int read_cluster(uint32_t c) {
    if (c < 2 || c >= ncl + 2) return -1;
    return dev->read(dev->ctx, data_lba + (uint64_t)(c - 2) * spc, spc, cl_buf) ? -3 : 0;
}

static uint32_t next_cl(uint32_t c) {                      /* 0 — конец цепочки или ошибка */
    uint64_t lba = fat_lba + (c * 4) / 512;
    if (lba != fat_sec_lba) { if (dev->read(dev->ctx, lba, 1, fat_sec)) return 0; fat_sec_lba = lba; }
    uint32_t v = rd32(fat_sec + (c * 4) % 512) & 0x0FFFFFFF;
    return (v < 2 || v >= ncl + 2) ? 0 : v;
}

struct ent { char name[260]; int is_dir; uint32_t size, cl; };

static int put_utf8(char *o, uint32_t cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); return 3;
}

static uint8_t sfn_sum(const uint8_t *n) { uint8_t s = 0; for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + n[i]); return s; }

/* Обходит каталог; cb возвращает ненулевое, чтобы остановиться. Возвращает: 0 обошли, 1 остановились, <0 ошибка. */
static int dir_walk(uint32_t cl, int (*cb)(const struct ent *, void *), void *ctx) {
    uint16_t lfn[260]; int have_lfn = 0, lfn_sum = 0;
    uint32_t guard = 0;
    while (cl) {
        if (++guard > ncl + 2) return -3;                  /* цепочка зациклена */
        if (read_cluster(cl)) return -3;
        for (uint32_t off = 0; off < spc * 512; off += 32) {
            const uint8_t *e = cl_buf + off;
            if (e[0] == 0x00) return 0;
            if (e[0] == 0xE5) { have_lfn = 0; continue; }
            if (e[11] == 0x0F) {                           /* кусок длинного имени */
                int seq = e[0] & 0x1F;
                if (e[0] & 0x40) { have_lfn = 1; lfn_sum = e[13]; for (int i = 0; i < 260; i++) lfn[i] = 0; }
                if (!have_lfn || seq < 1 || seq > 20 || e[13] != lfn_sum) { have_lfn = 0; continue; }
                static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                for (int i = 0; i < 13; i++) lfn[(seq - 1) * 13 + i] = (uint16_t)rd16(e + pos[i]);
                continue;
            }
            if (e[11] & 0x08) { have_lfn = 0; continue; }   /* метка тома */
            struct ent en;
            int k = 0;
            if (have_lfn && sfn_sum(e) == lfn_sum) {
                for (int i = 0; i < 255 && lfn[i] && lfn[i] != 0xFFFF; i++) k += put_utf8(en.name + k, lfn[i]);
            } else {
                for (int i = 0; i < 8 && e[i] != ' '; i++) en.name[k++] = (char)((i == 0 && e[0] == 0x05) ? 0xE5 : e[i]);
                if (e[8] != ' ') { en.name[k++] = '.'; for (int i = 8; i < 11 && e[i] != ' '; i++) en.name[k++] = (char)e[i]; }
            }
            en.name[k] = 0; have_lfn = 0;
            en.is_dir = (e[11] & 0x10) != 0; en.size = rd32(e + 28);
            en.cl = rd16(e + 20) << 16 | rd16(e + 26);
            if (strcmp(en.name, ".") == 0 || strcmp(en.name, "..") == 0) continue;
            if (cb(&en, ctx)) return 1;
        }
        cl = next_cl(cl);
    }
    return 0;
}

static uint32_t u8next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = s[0]; int n = 1;
    if (c >= 0xE0 && s[1] && s[2]) { c = ((c & 15) << 12) | ((s[1] & 63) << 6) | (s[2] & 63); n = 3; }
    else if (c >= 0xC0 && s[1]) { c = ((c & 31) << 6) | (s[1] & 63); n = 2; }
    *p += n;
    return c;
}
static uint32_t fold(uint32_t c) {                  /* A-Z и русские буквы без учёта регистра */
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c == 0x401) return 0x451;
    return c;
}
static int ieq(const char *a, const char *b) {
    while (*a && *b) if (fold(u8next(&a)) != fold(u8next(&b))) return 0;
    return *a == *b;
}

struct find { const char *name; struct ent *out; };
static int find_cb(const struct ent *e, void *c) {
    struct find *f = c;
    if (!ieq(e->name, f->name)) return 0;
    *f->out = *e; return 1;
}

/* path -> запись; пустой путь = корень (is_dir=1) */
static int resolve(const char *path, struct ent *out) {
    if (!mounted) return -1;
    out->is_dir = 1; out->cl = root_cl; out->size = 0; out->name[0] = 0;
    while (*path == '/') path++;
    while (*path) {
        char part[130]; size_t n = 0;
        while (*path && *path != '/') { if (n < sizeof part - 1) part[n++] = *path; path++; }
        part[n] = 0;
        while (*path == '/') path++;
        if (!out->is_dir) return -1;
        struct ent found; struct find f = { part, &found };
        int r = dir_walk(out->cl ? out->cl : root_cl, find_cb, &f);
        if (r < 0) return r;
        if (r == 0) return -1;
        *out = found;
        if (out->is_dir && out->cl == 0) out->cl = root_cl;      /* ".." на корень */
    }
    return 0;
}

struct lsctx { fat_ls_cb cb; void *ctx; };
static int ls_cb(const struct ent *e, void *c) { struct lsctx *l = c; l->cb(e->name, e->is_dir, e->size, l->ctx); return 0; }

int fat_list(const char *path, fat_ls_cb cb, void *ctx) {
    struct ent e;
    int r = resolve(path, &e);
    if (r < 0) return r;
    if (!e.is_dir) return -1;
    struct lsctx l = { cb, ctx };
    r = dir_walk(e.cl, ls_cb, &l);
    return r < 0 ? r : 0;
}

int fat_read(const char *path, uint8_t **data, size_t *len, size_t max) {
    struct ent e;
    int r = resolve(path, &e);
    if (r < 0) return r == -3 ? -3 : -1;
    if (e.is_dir) return -1;
    if (e.size > max) return -2;
    uint8_t *buf = kmalloc((size_t)e.size + 1);
    if (!buf) return -2;
    size_t got = 0; uint32_t cl = e.cl, guard = 0;
    while (got < e.size && cl) {
        if (++guard > ncl + 2 || read_cluster(cl)) { kfree(buf); return -3; }
        size_t n = e.size - got < (size_t)spc * 512 ? e.size - got : (size_t)spc * 512;
        memcpy(buf + got, cl_buf, n);
        got += n;
        cl = next_cl(cl);
    }
    if (got != e.size) { kfree(buf); return -3; }
    buf[got] = 0;
    *data = buf; *len = got;
    return 0;
}
