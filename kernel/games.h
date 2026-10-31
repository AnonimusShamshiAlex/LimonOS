#ifndef LIMON_GAMES_H
#define LIMON_GAMES_H
#include <stdint.h>
#include <stddef.h>

/* Игры LimonOS. Логика (games.c) не знает про экран — её можно проверять на обычном компьютере;
   рисование — в games_draw.c. Время считается в тиках таймера (100 в секунду). */
enum { GAME_SNAKE, GAME_TETRIS, GAME_PONG, GAME_2048, GAME_COUNT };

#define SNAKE_W 24
#define SNAKE_H 18
#define SNAKE_MAX (SNAKE_W * SNAKE_H)

struct snake_st {
    uint8_t x[SNAKE_MAX], y[SNAKE_MAX];
    int len, dir, queued[2], nq, fx, fy;
    int bonus_x, bonus_y; uint64_t bonus_until;      /* редкая «розовая» ягода: +50 очков, пропадает */
};

#define TET_W 10
#define TET_H 20
struct tetris_st {
    uint8_t board[TET_H][TET_W];                     /* 0 пусто, 1..7 — цвет фигуры */
    int piece, rot, px, py, next;
    int lines, level, bag[7], bag_pos;
    int flash;                                       /* сколько строк только что сгорело (для мигания) */
};

#define PONG_W 400
#define PONG_H 260
#define PONG_PADH 56
struct pong_st {
    int ball_x, ball_y, vx, vy;                      /* x, y — в 1/256 пикселя */
    int py, ay;                                      /* верх ракеток */
    int ps, as;                                      /* счёт игрока и компьютера */
    int serve_wait;                                  /* тиков до подачи */
    int rally;
};

struct g2048_st { uint16_t cell[4][4]; int moved_flash; int won; };

struct game {
    int kind;
    int over, paused, fresh, score, best[GAME_COUNT];   /* fresh: игра ещё не начиналась, ждёт первую клавишу */
    int dirty;                                       /* 1 — картинку нужно перерисовать */
    uint32_t rng;
    uint64_t next_step, started;
    union { struct snake_st sn; struct tetris_st te; struct pong_st po; struct g2048_st g; } u;
};

void        game_init(struct game *g, int kind, uint32_t seed, uint64_t now);
void        game_key(struct game *g, uint32_t cp, uint8_t mods, uint64_t now);
int         game_tick(struct game *g, uint64_t now);       /* 1 — состояние изменилось */
void        game_mouse_y(struct game *g, int y);           /* Понг: ракетка за мышью (y внутри поля) */
const char *game_title(int kind);
const char *game_help(int kind);
void        game_size(int kind, int *w, int *h);           /* размер рабочей области окна в пикселях */

/* рисование: цвета берутся из темы рабочего стола */
struct game_pal { uint32_t bg, panel, fg, dim, accent, accent2, good, bad; };
void game_draw(const struct game *g, int x, int y, const struct game_pal *pal);

/* для тестов */
int  tetris_piece_cells(int piece, int rot, int out[4][2]);
int  tetris_ghost_y(const struct game *g);                    /* куда упадёт фигура (для «призрака») */
#endif
