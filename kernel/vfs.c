#include "vfs.h"
#include "mem.h"
#include "kstring.h"
#include "format.h"

#define VFS_MAX 160
#define VFS_NAME_MAX 48

struct node { char name[VFS_NAME_MAX]; int used, is_dir, parent; uint8_t *data; size_t len; };
static struct node nodes[VFS_MAX];
static int nnodes;    /* верхняя граница занятых слотов (для поиска свободных ниже неё) */

int vfs_desktop, vfs_downloads, vfs_music, vfs_pictures, vfs_system;

int vfs_name_ok(const char *name) {
    if (!name || !name[0]) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    for (const char *p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\') return 0;
    }
    return strlen(name) < VFS_NAME_MAX;
}

static int alloc_node(void) {
    for (int i = 0; i < VFS_MAX; i++) if (!nodes[i].used) { if (i >= nnodes) nnodes = i + 1; return i; }
    return VFS_NONE;
}

static int child_of(int dir, const char *name) {
    for (int i = 0; i < nnodes; i++)
        if (nodes[i].used && nodes[i].parent == dir && strcmp(nodes[i].name, name) == 0) return i;
    return VFS_NONE;
}

int vfs_is_dir(int idx) { return idx >= 0 && idx < nnodes && nodes[idx].used && nodes[idx].is_dir; }
int vfs_parent(int idx) { return (idx >= 0 && idx < nnodes && nodes[idx].used) ? nodes[idx].parent : VFS_NONE; }
const char *vfs_name(int idx) { return (idx >= 0 && idx < nnodes && nodes[idx].used) ? nodes[idx].name : ""; }
const uint8_t *vfs_data(int idx) { return (idx >= 0 && idx < nnodes && nodes[idx].used && !nodes[idx].is_dir) ? nodes[idx].data : NULL; }
size_t vfs_len(int idx) { return (idx >= 0 && idx < nnodes && nodes[idx].used && !nodes[idx].is_dir) ? nodes[idx].len : 0; }

int vfs_mkdir(int dir, const char *name) {
    if (!vfs_is_dir(dir) || !vfs_name_ok(name) || child_of(dir, name) != VFS_NONE) return VFS_NONE;
    int i = alloc_node();
    if (i == VFS_NONE) return VFS_NONE;
    memset(&nodes[i], 0, sizeof nodes[i]);
    ksnprintf(nodes[i].name, sizeof nodes[i].name, "%s", name);
    nodes[i].used = 1; nodes[i].is_dir = 1; nodes[i].parent = dir;
    return i;
}

int vfs_put(int dir, const char *name, uint8_t *data, size_t len) {
    if (!vfs_is_dir(dir) || !vfs_name_ok(name)) return VFS_NONE;
    int i = child_of(dir, name);
    if (i != VFS_NONE) {
        if (nodes[i].is_dir) return VFS_NONE;            /* нельзя заменить папку файлом */
        if (nodes[i].data) kfree(nodes[i].data);
    } else {
        i = alloc_node();
        if (i == VFS_NONE) return VFS_NONE;
        memset(&nodes[i], 0, sizeof nodes[i]);
        ksnprintf(nodes[i].name, sizeof nodes[i].name, "%s", name);
        nodes[i].used = 1; nodes[i].parent = dir;
    }
    nodes[i].is_dir = 0; nodes[i].data = data; nodes[i].len = len;
    return i;
}

static int has_children(int dir) {
    for (int i = 0; i < nnodes; i++) if (nodes[i].used && nodes[i].parent == dir) return 1;
    return 0;
}

int vfs_delete(int idx) {
    if (idx <= VFS_ROOT || idx >= nnodes || !nodes[idx].used) return -1;     /* корень не трогаем */
    if (nodes[idx].is_dir && has_children(idx)) return -1;
    if (!nodes[idx].is_dir && nodes[idx].data) kfree(nodes[idx].data);
    memset(&nodes[idx], 0, sizeof nodes[idx]);
    return 0;
}

static int is_ancestor(int a, int of) {              /* 1, если a — предок of (или a==of) */
    for (int i = of; i != VFS_NONE; i = nodes[i].parent) if (i == a) return 1;
    return 0;
}

int vfs_rename(int idx, int new_dir, const char *new_name) {
    if (idx <= VFS_ROOT || idx >= nnodes || !nodes[idx].used || !vfs_is_dir(new_dir) || !vfs_name_ok(new_name)) return -1;
    if (nodes[idx].is_dir && is_ancestor(idx, new_dir)) return -1;           /* нельзя переместить папку в саму себя */
    if (child_of(new_dir, new_name) != VFS_NONE) return -1;
    ksnprintf(nodes[idx].name, sizeof nodes[idx].name, "%s", new_name);
    nodes[idx].parent = new_dir;
    return 0;
}

int vfs_copy(int idx, int new_dir, const char *new_name) {
    if (idx < 0 || idx >= nnodes || !nodes[idx].used || nodes[idx].is_dir) return VFS_NONE;
    uint8_t *cp = kmalloc(nodes[idx].len ? nodes[idx].len : 1);
    if (!cp) return VFS_NONE;
    memcpy(cp, nodes[idx].data, nodes[idx].len);
    int r = vfs_put(new_dir, new_name, cp, nodes[idx].len);
    if (r == VFS_NONE) kfree(cp);
    return r;
}

int vfs_resolve(int cwd, const char *path) {
    if (!path || !path[0]) return cwd;
    int cur = (path[0] == '/') ? VFS_ROOT : cwd;
    if (path[0] == '/') path++;
    while (*path) {
        char part[VFS_NAME_MAX]; size_t n = 0;
        while (*path && *path != '/' && n < sizeof part - 1) part[n++] = *path++;
        part[n] = 0;
        while (*path == '/') path++;
        if (!n || strcmp(part, ".") == 0) continue;
        if (strcmp(part, "..") == 0) { if (cur != VFS_ROOT) cur = nodes[cur].parent; continue; }
        if (!vfs_is_dir(cur)) return VFS_NONE;
        cur = child_of(cur, part);
        if (cur == VFS_NONE) return VFS_NONE;
    }
    return cur;
}

void vfs_path(int idx, char *out, size_t cap) {
    if (idx == VFS_ROOT || idx == VFS_NONE) { ksnprintf(out, cap, "/"); return; }
    char stack[8][VFS_NAME_MAX]; int n = 0;
    for (int i = idx; i != VFS_ROOT && i != VFS_NONE && n < 8; i = nodes[i].parent) ksnprintf(stack[n++], VFS_NAME_MAX, "%s", nodes[i].name);
    size_t o = 0;
    for (int i = n - 1; i >= 0; i--) o += (size_t)ksnprintf(out + o, o < cap ? cap - o : 0, "/%s", stack[i]);
    if (!n) ksnprintf(out, cap, "/");
}

int vfs_list(int dir, vfs_ls_cb cb, void *ctx) {
    if (!vfs_is_dir(dir)) return -1;
    for (int i = 0; i < nnodes; i++)
        if (nodes[i].used && nodes[i].parent == dir) cb(i, nodes[i].name, nodes[i].is_dir, nodes[i].len, ctx);
    return 0;
}

size_t vfs_total_bytes(void) {
    size_t s = 0;
    for (int i = 0; i < nnodes; i++) if (nodes[i].used && !nodes[i].is_dir) s += nodes[i].len;
    return s;
}

static void put_text(int dir, const char *name, const char *text) {
    size_t n = strlen(text);
    uint8_t *d = kmalloc(n);
    if (!d) return;
    memcpy(d, text, n);
    vfs_put(dir, name, d, n);
}

int vfs_init(void) {
    memset(nodes, 0, sizeof nodes);
    nnodes = 0;
    int r = alloc_node();                         /* узел 0 — корень */
    if (r != VFS_ROOT) return -1;
    memset(&nodes[VFS_ROOT], 0, sizeof nodes[VFS_ROOT]);
    nodes[VFS_ROOT].used = 1; nodes[VFS_ROOT].is_dir = 1; nodes[VFS_ROOT].parent = VFS_NONE;   /* у корня нет родителя: иначе list(root) нашёл бы сам корень как свой же child (parent==0==VFS_ROOT) и is_ancestor() зациклился бы */
    ksnprintf(nodes[VFS_ROOT].name, sizeof nodes[VFS_ROOT].name, "/");
    vfs_desktop   = vfs_mkdir(VFS_ROOT, "Desktop");
    vfs_downloads = vfs_mkdir(VFS_ROOT, "Downloads");
    vfs_music     = vfs_mkdir(VFS_ROOT, "Music");
    vfs_pictures  = vfs_mkdir(VFS_ROOT, "Pictures");
    vfs_system    = vfs_mkdir(VFS_ROOT, "System");
    if (vfs_system != VFS_NONE) {
        put_text(vfs_system, "about.txt",
            "LimonOS — своя ОС с нуля (x86_64), не Linux и не совместима с ним на уровне программ.\n"
            "Эти файлы и папки живут в оперативной памяти и пропадают при выключении:\n"
            "запись на диск (FAT32) ещё не сделана, FAT32 пока только читается (disk, dload).\n");
    }
    return 0;
}
