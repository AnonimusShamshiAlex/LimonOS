#ifndef LIMON_DESKTOP_H
#define LIMON_DESKTOP_H
#include <stdint.h>
#include "keyboard.h"
#include "mouse.h"

void desktop_init(void);
void desktop_run(void);                          /* главный цикл, не возвращается */
void desktop_key(const struct key_event *ev);
void desktop_mouse(const struct mouse_event *ev);
void desktop_tick(void);                         /* мигание курсора, часы */
void desktop_refresh(void);                      /* перерисовать прямо сейчас (для долгих команд) */
void desktop_frame(void);                        /* перерисовать, если что-то изменилось */

int  desktop_open_terminal(void);                /* -1, если места нет */
void desktop_open_about(void);
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
