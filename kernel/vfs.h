#ifndef LIMON_VFS_H
#define LIMON_VFS_H
#include <stdint.h>
#include <stddef.h>

/* Memoria — файлы и папки LimonOS в оперативной памяти (живут до выключения; на диск не пишутся —
   запись на FAT32 не сделана). Так в системе появляются настоящие Desktop/Downloads/Music/Pictures,
   а не один плоский список файлов. Корень создаёт vfs_init(). Узлы — по индексу (int), 0 — корень. */
#define VFS_ROOT 0
#define VFS_NONE (-1)

int  vfs_init(void);                               /* 0 — успех; создаёт корень и системные папки */
int  vfs_name_ok(const char *name);                /* 1 — допустимое имя (без / \ и управляющих, не . и не ..) */
int  vfs_is_dir(int idx);
int  vfs_resolve(int cwd, const char *path);        /* cwd + относительный/абсолютный путь -> узел; VFS_NONE если нет */
int  vfs_mkdir(int dir, const char *name);           /* -> новый узел или VFS_NONE */
int  vfs_put(int dir, const char *name, uint8_t *data, size_t len);   /* забирает владение data (kmalloc); заменяет файл; -> узел или VFS_NONE */
int  vfs_delete(int idx);                            /* 0 — успех (папку удаляет, только если пуста) */
int  vfs_rename(int idx, int new_dir, const char *new_name);   /* перемещение и/или переименование; 0 — успех */
int  vfs_copy(int idx, int new_dir, const char *new_name);     /* копирование файла (не папки); -> новый узел или VFS_NONE */
const char   *vfs_name(int idx);
const uint8_t *vfs_data(int idx);
size_t        vfs_len(int idx);
int           vfs_parent(int idx);
void          vfs_path(int idx, char *out, size_t cap);         /* "/Загрузки/файл.txt" */
typedef void (*vfs_ls_cb)(int idx, const char *name, int is_dir, size_t size, void *ctx);
int  vfs_list(int dir, vfs_ls_cb cb, void *ctx);     /* 0 — успех, -1 — не папка */
size_t vfs_total_bytes(void);

/* удобные указатели на системные папки, заполняются vfs_init() */
extern int vfs_desktop, vfs_downloads, vfs_music, vfs_pictures, vfs_system;
#endif
