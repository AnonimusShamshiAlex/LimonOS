#ifndef LIMON_DESKTOP_H
#define LIMON_DESKTOP_H
#include <stdint.h>
#include "keyboard.h"
#include "mouse.h"
#include <stddef.h>

void desktop_init(void);
void desktop_run(void);                          /* главный цикл, не возвращается */
void desktop_key(const struct key_event *ev);
void desktop_mouse(const struct mouse_event *ev);
void desktop_tick(void);                         /* мигание курсора, часы */
void desktop_refresh(void);                      /* перерисовать прямо сейчас (для долгих команд) */
void desktop_frame(void);                        /* перерисовать, если что-то изменилось */

int  desktop_open_terminal(void);                /* -1, если места нет */
void desktop_open_about(void);
int  desktop_open_browser(const char *text);     /* адрес, слова для поиска или пусто (домашняя страница); -1, если места нет; блокирует на время загрузки */
#define DESKTOP_MAX_BROWSERS 3
int  desktop_open_game(int kind);                /* 0 змейка, 1 тетрис, 2 понг, 3 2048; -1 — нет места */
int  desktop_open_files(int start_dir);          /* -1 — корень Memoria */
int  desktop_open_taskmgr(void);
int  desktop_task_count(void);
int  desktop_task_info(int ord, char *title, size_t tcap, char *kind, size_t kcap, int *minimized, int *focused);  /* 0 — успех */
int  desktop_task_close(int ord);                /* 0 — закрыто */
void desktop_power(int reboot);                  /* 1 — перезагрузка, 0 — выключение */

/* темы оформления */
int         desktop_theme_count(void);
int         desktop_theme_index(void);
int         desktop_theme_find(const char *name);
void        desktop_set_theme(int idx);
const char *desktop_theme_name(void);
const char *desktop_theme_name_at(int idx);
uint32_t    desktop_theme_accent(int idx);

#endif
