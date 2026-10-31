#ifndef LIMON_STORE_H
#define LIMON_STORE_H
#include <stdint.h>
#include <stddef.h>

/* Basket — хранилище скачанных файлов в оперативной памяти (пока нет диска).
   Файлы живут до выключения. Имя файла — до 47 символов. */
#define STORE_MAX 24
struct store_file { char name[48]; uint8_t *data; size_t len; int used; };

int  store_name_ok(const char *name);                             /* 1 — имя допустимо */
int  store_put(const char *name, uint8_t *data, size_t len);   /* забирает владение data (kmalloc); заменяет файл с тем же именем; 0 — успех, -1 нет места, -2 плохое имя */
const struct store_file *store_get(const char *name);
const struct store_file *store_at(int i);                      /* i-й занятый слот или NULL */
int  store_drop(const char *name);                             /* 0 — удалён */
size_t store_total(void);
#endif
