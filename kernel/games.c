/* Логика игр: Змейка, Тетрис, Понг, 2048. Без графики и без ядра — только числа. */
#include "games.h"
#include "keyboard.h"

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint32_t rnd(struct game *g) {          /* xorshift32 */
    uint32_t x = g->rng ? g->rng : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g->rng = x;
    return x;
}
static int rnd_n(struct game *g, int n) { return (int)(rnd(g) % (uint32_t)n); }

static int is_left(uint32_t c)  { return c == KEY_LEFT  || c == 'a' || c == 'A' || c == 0x444 || c == 0x424; }   /* ф Ф */
static int is_right(uint32_t c) { return c == KEY_RIGHT || c == 'd' || c == 'D' || c == 0x432 || c == 0x412; }   /* в В */
static int is_up(uint32_t c)    { return c == KEY_UP    || c == 'w' || c == 'W' || c == 0x446 || c == 0x426; }   /* ц Ц */
static int is_down(uint32_t c)  { return c == KEY_DOWN  || c == 's' || c == 'S' || c == 0x44B || c == 0x42B; }   /* ы Ы */
static int is_pause(uint32_t c) { return c == 'p' || c == 'P' || c == 0x437 || c == 0x417; }                      /* з З */
static int is_restart(uint32_t c) { return c == 'r' || c == 'R' || c == 0x43A || c == 0x41A; }                    /* к К */

/* ======================= ЗМЕЙКА ======================= */
static const int DX[4] = { 0, 1, 0, -1 }, DY[4] = { -1, 0, 1, 0 };     /* 0 вверх, 1 вправо, 2 вниз, 3 влево */

static int snake_hits(const struct snake_st *s, int x, int y, int skip_tail) {
    int n = s->len - (skip_tail ? 1 : 0);
    for (int i = 0; i < n; i++) if (s->x[i] == x && s->y[i] == y) return 1;
    return 0;
}

static void snake_food(struct game *g) {
    struct snake_st *s = &g->u.sn;
    if (s->len >= SNAKE_MAX) { s->fx = -1; return; }
    for (int tries = 0; tries < 2000; tries++) {
        int x = rnd_n(g, SNAKE_W), y = rnd_n(g, SNAKE_H);
        if (!snake_hits(s, x, y, 0) && !(x == s->bonus_x && y == s->bonus_y)) { s->fx = x; s->fy = y; return; }
    }
    for (int y = 0; y < SNAKE_H; y++) for (int x = 0; x < SNAKE_W; x++)       /* запасной путь: первая свободная клетка */
        if (!snake_hits(s, x, y, 0)) { s->fx = x; s->fy = y; return; }
}

static int snake_interval(const struct game *g) {
    int iv = 14 - g->score / 60;                  /* тики между шагами; со счётом быстрее */
    return iv < 5 ? 5 : iv;
}

static void snake_init(struct game *g, uint64_t now) {
    struct snake_st *s = &g->u.sn;
    s->len = 4; s->dir = 1; s->nq = 0;
    for (int i = 0; i < s->len; i++) { s->x[i] = (uint8_t)(SNAKE_W / 2 - i); s->y[i] = SNAKE_H / 2; }   /* [0] — голова */
    s->bonus_x = s->bonus_y = -1; s->bonus_until = 0;
    snake_food(g);
    g->next_step = now + (uint64_t)snake_interval(g);
}

static void snake_key(struct game *g, uint32_t c) {
    struct snake_st *s = &g->u.sn;
    int nd = -1;
    if (is_up(c)) nd = 0; else if (is_right(c)) nd = 1; else if (is_down(c)) nd = 2; else if (is_left(c)) nd = 3;
    if (nd < 0) return;
    int last = s->nq ? s->queued[s->nq - 1] : s->dir;
    if (nd == last || nd == ((last + 2) & 3)) return;            /* назад и «туда же» не нужно */
    if (s->nq < 2) s->queued[s->nq++] = nd;                      /* очередь из двух поворотов: быстрые нажатия не теряются */
}

static int snake_step(struct game *g, uint64_t now) {
    struct snake_st *s = &g->u.sn;
    if (s->nq) { s->dir = s->queued[0]; s->queued[0] = s->queued[1]; s->nq--; }
    int nx = s->x[0] + DX[s->dir], ny = s->y[0] + DY[s->dir];
    if (nx < 0 || ny < 0 || nx >= SNAKE_W || ny >= SNAKE_H) { g->over = 1; return 1; }
    int eat = (nx == s->fx && ny == s->fy);
    int bon = (nx == s->bonus_x && ny == s->bonus_y);
    if (snake_hits(s, nx, ny, !eat && !bon)) { g->over = 1; return 1; }   /* хвост освободится — в него можно */
    int grow = eat || bon;
    if (grow && s->len < SNAKE_MAX) s->len++;
    for (int i = s->len - 1; i > 0; i--) { s->x[i] = s->x[i - 1]; s->y[i] = s->y[i - 1]; }
    s->x[0] = (uint8_t)nx; s->y[0] = (uint8_t)ny;
    if (eat) { g->score += 10; snake_food(g); if (s->bonus_x < 0 && rnd_n(g, 4) == 0) {
        for (int t = 0; t < 200; t++) { int x = rnd_n(g, SNAKE_W), y = rnd_n(g, SNAKE_H);
            if (!snake_hits(s, x, y, 0) && !(x == s->fx && y == s->fy)) { s->bonus_x = x; s->bonus_y = y; s->bonus_until = now + 600; break; } } } }
    if (bon) { g->score += 50; s->bonus_x = s->bonus_y = -1; }
    if (s->bonus_x >= 0 && now >= s->bonus_until) s->bonus_x = s->bonus_y = -1;
    if (s->len >= SNAKE_MAX) g->over = 1;                        /* всё поле занято — победа */
    return 1;
}

/* ======================= ТЕТРИС ======================= */
/* Фигуры: I O T S Z J L. Маски 4x4 (бит y*4+x). Вращение — в квадрате n×n (3 для большинства, 4 для I). */
static const uint16_t TET_MASK[7] = { 0x00F0, 0x0660, 0x0072, 0x0036, 0x0063, 0x0071, 0x0074 };
static const int TET_N[7] = { 4, 4, 3, 3, 3, 3, 3 };

static uint16_t tet_rot_mask(int piece, int rot) {
    uint16_t m = TET_MASK[piece];
    if (piece == 1) return m;                                   /* O не вращается */
    int n = TET_N[piece];
    for (int r = 0; r < (rot & 3); r++) {
        uint16_t o = 0;
        for (int y = 0; y < n; y++) for (int x = 0; x < n; x++)
            if (m & (1u << (y * 4 + x))) o |= (uint16_t)(1u << (x * 4 + (n - 1 - y)));
        m = o;
    }
    return m;
}

int tetris_piece_cells(int piece, int rot, int out[4][2]) {
    uint16_t m = tet_rot_mask(piece, rot);
    int k = 0;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
        if ((m & (1u << (y * 4 + x))) && k < 4) { out[k][0] = x; out[k][1] = y; k++; }
    return k;
}

static int tet_fits(const struct tetris_st *t, int piece, int rot, int px, int py) {
    int c[4][2];
    tetris_piece_cells(piece, rot, c);
    for (int i = 0; i < 4; i++) {
        int x = px + c[i][0], y = py + c[i][1];
        if (x < 0 || x >= TET_W || y >= TET_H) return 0;
        if (y >= 0 && t->board[y][x]) return 0;
    }
    return 1;
}

static int tet_next_from_bag(struct game *g) {
    struct tetris_st *t = &g->u.te;
    if (t->bag_pos >= 7) {                                      /* «мешок семи»: все фигуры по разу, потом заново */
        for (int i = 0; i < 7; i++) t->bag[i] = i;
        for (int i = 6; i > 0; i--) { int j = rnd_n(g, i + 1); int tmp = t->bag[i]; t->bag[i] = t->bag[j]; t->bag[j] = tmp; }
        t->bag_pos = 0;
    }
    return t->bag[t->bag_pos++];
}

static int tet_interval(const struct tetris_st *t) {
    int iv = 50 - t->level * 4;                                 /* тиков на клетку падения */
    return iv < 4 ? 4 : iv;
}

static void tet_spawn(struct game *g, uint64_t now) {
    struct tetris_st *t = &g->u.te;
    t->piece = t->next; t->next = tet_next_from_bag(g);
    t->rot = 0; t->px = 3; t->py = (t->piece == 0) ? -1 : 0;
    if (!tet_fits(t, t->piece, t->rot, t->px, t->py)) g->over = 1;
    g->next_step = now + (uint64_t)tet_interval(t);
}

static void tetris_init(struct game *g, uint64_t now) {
    struct tetris_st *t = &g->u.te;
    t->bag_pos = 7; t->lines = 0; t->level = 0;
    t->next = tet_next_from_bag(g);
    tet_spawn(g, now);
}

static void tet_lock(struct game *g, uint64_t now) {
    struct tetris_st *t = &g->u.te;
    int c[4][2];
    tetris_piece_cells(t->piece, t->rot, c);
    for (int i = 0; i < 4; i++) {
        int x = t->px + c[i][0], y = t->py + c[i][1];
        if (y < 0) { g->over = 1; return; }                     /* зафиксировалась выше поля — конец */
        t->board[y][x] = (uint8_t)(t->piece + 1);
    }
    int cleared = 0;
    for (int y = TET_H - 1; y >= 0; ) {
        int full = 1;
        for (int x = 0; x < TET_W; x++) if (!t->board[y][x]) { full = 0; break; }
        if (!full) { y--; continue; }
        for (int yy = y; yy > 0; yy--) for (int x = 0; x < TET_W; x++) t->board[yy][x] = t->board[yy - 1][x];
        for (int x = 0; x < TET_W; x++) t->board[0][x] = 0;
        cleared++;                                              /* ту же строку проверяем снова: на её место упала верхняя */
    }
    static const int pts[5] = { 0, 100, 300, 500, 800 };
    g->score += pts[cleared] * (t->level + 1);
    t->lines += cleared; t->flash = cleared;
    t->level = t->lines / 10;
    tet_spawn(g, now);
}

static int tet_move(struct game *g, int dx, int dy) {
    struct tetris_st *t = &g->u.te;
    if (!tet_fits(t, t->piece, t->rot, t->px + dx, t->py + dy)) return 0;
    t->px += dx; t->py += dy;
    return 1;
}

static void tet_rotate(struct game *g) {
    struct tetris_st *t = &g->u.te;
    int nr = (t->rot + 1) & 3;
    static const int kicks[5] = { 0, -1, 1, -2, 2 };            /* простой «пинок» от стенки */
    for (int i = 0; i < 5; i++)
        if (tet_fits(t, t->piece, nr, t->px + kicks[i], t->py)) { t->rot = nr; t->px += kicks[i]; return; }
}

static void tetris_key(struct game *g, uint32_t c, uint64_t now) {
    struct tetris_st *t = &g->u.te;
    if (is_left(c)) tet_move(g, -1, 0);
    else if (is_right(c)) tet_move(g, 1, 0);
    else if (is_up(c)) tet_rotate(g);
    else if (is_down(c)) { if (tet_move(g, 0, 1)) { g->score += 1; g->next_step = now + (uint64_t)tet_interval(t); } else tet_lock(g, now); }
    else if (c == ' ') {                                        /* жёсткий сброс */
        int d = 0;
        while (tet_move(g, 0, 1)) d++;
        g->score += d * 2;
        tet_lock(g, now);
    }
}

int tetris_ghost_y(const struct game *g) {
    const struct tetris_st *t = &g->u.te;
    int y = t->py;
    while (tet_fits(t, t->piece, t->rot, t->px, y + 1)) y++;
    return y;
}

/* ======================= ПОНГ ======================= */
#define FX 256
static void pong_serve(struct game *g, int dir) {
    struct pong_st *p = &g->u.po;
    p->ball_x = (PONG_W / 2) * FX; p->ball_y = (PONG_H / 2) * FX;
    p->vx = dir * 3 * FX / 2; p->vy = (rnd_n(g, 5) - 2) * FX / 2;
    if (p->vy == 0) p->vy = FX / 2;
    p->rally = 0; p->serve_wait = 60;
}

static void pong_init(struct game *g, uint64_t now) {
    struct pong_st *p = &g->u.po;
    p->py = p->ay = (PONG_H - PONG_PADH) / 2;
    p->ps = p->as = 0;
    pong_serve(g, 1);
    g->next_step = now + 2;
}

void game_mouse_y(struct game *g, int y) {
    if (g->kind != GAME_PONG || g->over || g->paused) return;
    int v = y - PONG_PADH / 2;
    v = clampi(v, 0, PONG_H - PONG_PADH);
    g->u.po.py = v; g->dirty = 1;
}

static void pong_key(struct game *g, uint32_t c) {
    struct pong_st *p = &g->u.po;
    if (is_up(c)) p->py -= 16; else if (is_down(c)) p->py += 16; else return;
    p->py = clampi(p->py, 0, PONG_H - PONG_PADH);
}

static void pong_step(struct game *g) {
    struct pong_st *p = &g->u.po;
    if (p->serve_wait > 0) { p->serve_wait--; return; }
    /* компьютер: идёт к мячу не быстрее 2 пикселей за шаг и с «ленью», если мяч далеко */
    int target = p->ball_y / FX - PONG_PADH / 2;
    int diff = target - p->ay;
    int maxs = 2;
    if (p->vx < 0) { maxs = 1; diff = ((PONG_H - PONG_PADH) / 2) - p->ay; diff /= 3; }   /* мяч летит от него — возвращается к центру */
    diff = clampi(diff, -maxs, maxs);
    p->ay = clampi(p->ay + diff, 0, PONG_H - PONG_PADH);

    p->ball_x += p->vx; p->ball_y += p->vy;
    if (p->ball_y < 4 * FX) { p->ball_y = 4 * FX; p->vy = -p->vy; }
    if (p->ball_y > (PONG_H - 4) * FX) { p->ball_y = (PONG_H - 4) * FX; p->vy = -p->vy; }

    int bx = p->ball_x / FX, by = p->ball_y / FX;
    if (p->vx < 0 && bx <= 20 && bx >= 8 && by >= p->py - 4 && by <= p->py + PONG_PADH + 4) {          /* ракетка игрока */
        int off = by - (p->py + PONG_PADH / 2);                 /* где попали: от края зависит угол */
        p->vx = -p->vx + FX / 16; p->vy = off * FX / 14;
        p->ball_x = 21 * FX; p->rally++;
    } else if (p->vx > 0 && bx >= PONG_W - 20 && bx <= PONG_W - 8 && by >= p->ay - 4 && by <= p->ay + PONG_PADH + 4) {
        int off = by - (p->ay + PONG_PADH / 2);
        p->vx = -p->vx - FX / 16; p->vy = off * FX / 14;
        p->ball_x = (PONG_W - 21) * FX; p->rally++;
    }
    int lim = 5 * FX;
    p->vx = clampi(p->vx, -lim, lim);
    p->vy = clampi(p->vy, -3 * FX, 3 * FX);

    if (bx < -4) { p->as++; if (p->as >= 7) g->over = 1; else pong_serve(g, 1); }
    else if (bx > PONG_W + 4) { p->ps++; g->score += 10 + p->rally; if (p->ps >= 7) g->over = 1; else pong_serve(g, -1); }
}

/* ======================= 2048 ======================= */
static void g2048_spawn(struct game *g) {
    struct g2048_st *s = &g->u.g;
    int free_n = 0;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) if (!s->cell[y][x]) free_n++;
    if (!free_n) return;
    int k = rnd_n(g, free_n);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
        if (!s->cell[y][x] && k-- == 0) { s->cell[y][x] = rnd_n(g, 10) == 0 ? 4 : 2; return; }
}

static int g2048_can_move(const struct g2048_st *s) {
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        if (!s->cell[y][x]) return 1;
        if (x < 3 && s->cell[y][x] == s->cell[y][x + 1]) return 1;
        if (y < 3 && s->cell[y][x] == s->cell[y + 1][x]) return 1;
    }
    return 0;
}

/* сдвиг одной линии из 4 клеток к началу; возвращает 1, если что-то изменилось */
static int g2048_line(uint16_t *l, int *gain) {
    uint16_t out[4] = { 0, 0, 0, 0 };
    int n = 0, merged_last = 0;
    for (int i = 0; i < 4; i++) {
        if (!l[i]) continue;
        if (n > 0 && out[n - 1] == l[i] && !merged_last) { out[n - 1] = (uint16_t)(out[n - 1] * 2); *gain += out[n - 1]; merged_last = 1; }
        else { out[n++] = l[i]; merged_last = 0; }
    }
    int changed = 0;
    for (int i = 0; i < 4; i++) { if (l[i] != out[i]) changed = 1; l[i] = out[i]; }
    return changed;
}

static void g2048_move(struct game *g, int dir) {            /* 0 вверх 1 вправо 2 вниз 3 влево */
    struct g2048_st *s = &g->u.g;
    int changed = 0, gain = 0;
    for (int k = 0; k < 4; k++) {
        uint16_t l[4];
        for (int i = 0; i < 4; i++) {
            int x = dir == 0 || dir == 2 ? k : (dir == 3 ? i : 3 - i);
            int y = dir == 3 || dir == 1 ? k : (dir == 0 ? i : 3 - i);
            l[i] = s->cell[y][x];
        }
        changed |= g2048_line(l, &gain);
        for (int i = 0; i < 4; i++) {
            int x = dir == 0 || dir == 2 ? k : (dir == 3 ? i : 3 - i);
            int y = dir == 3 || dir == 1 ? k : (dir == 0 ? i : 3 - i);
            s->cell[y][x] = l[i];
        }
    }
    if (!changed) return;
    g->score += gain;
    g2048_spawn(g);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) if (s->cell[y][x] >= 2048) s->won = 1;
    if (!g2048_can_move(s)) g->over = 1;
}

static void g2048_init(struct game *g) { g2048_spawn(g); g2048_spawn(g); }

/* ======================= общий слой ======================= */
void game_init(struct game *g, int kind, uint32_t seed, uint64_t now) {
    int best[GAME_COUNT];
    for (int i = 0; i < GAME_COUNT; i++) best[i] = g->best[i];   /* рекорд переживает перезапуск */
    uint8_t *p = (uint8_t *)g;
    for (size_t i = 0; i < sizeof *g; i++) p[i] = 0;
    for (int i = 0; i < GAME_COUNT; i++) g->best[i] = best[i];
    g->kind = kind; g->rng = seed ? seed : 0x1234567u; g->started = now; g->dirty = 1;
    for (int i = 0; i < 4; i++) rnd(g);                          /* прогрев генератора */
    switch (kind) {
    case GAME_SNAKE:  snake_init(g, now); break;
    case GAME_TETRIS: tetris_init(g, now); break;
    case GAME_PONG:   pong_init(g, now); break;
    case GAME_2048:   g2048_init(g); break;
    default: break;
    }
    g->paused = 1; g->fresh = 1;                                  /* каждая игра ждёт первую клавишу — можно собраться */
}

void game_key(struct game *g, uint32_t cp, uint8_t mods, uint64_t now) {
    (void)mods;
    if (is_restart(cp)) { game_init(g, g->kind, g->rng ^ (uint32_t)now * 2654435761u, now); return; }
    if (g->over) { if (cp == '\n' || cp == ' ') game_init(g, g->kind, g->rng ^ (uint32_t)now * 2654435761u, now); return; }
    if (is_pause(cp)) {
        g->paused = !g->paused; g->fresh = 0;
        if (!g->paused) g->next_step = now + 2;                  /* после паузы не прыгаем сразу на несколько шагов */
        g->dirty = 1; return;
    }
    if (g->paused) { g->paused = 0; g->fresh = 0; g->next_step = now + 2; }   /* любая клавиша снимает паузу и сразу работает */
    g->dirty = 1;
    switch (g->kind) {
    case GAME_SNAKE:  snake_key(g, cp); break;
    case GAME_TETRIS: tetris_key(g, cp, now); break;
    case GAME_PONG:   pong_key(g, cp); break;
    case GAME_2048:
        if (is_up(cp)) g2048_move(g, 0); else if (is_right(cp)) g2048_move(g, 1);
        else if (is_down(cp)) g2048_move(g, 2); else if (is_left(cp)) g2048_move(g, 3);
        break;
    default: break;
    }
    if (g->score > g->best[g->kind]) g->best[g->kind] = g->score;
}

int game_tick(struct game *g, uint64_t now) {
    int changed = g->dirty; g->dirty = 0;
    if (g->over || g->paused) return changed;
    switch (g->kind) {
    case GAME_SNAKE:
        if (now >= g->next_step) {
            g->next_step = now + (uint64_t)snake_interval(g);
            snake_step(g, now); changed = 1;
        } else if (g->u.sn.bonus_x >= 0 && now >= g->u.sn.bonus_until) { g->u.sn.bonus_x = g->u.sn.bonus_y = -1; changed = 1; }
        break;
    case GAME_TETRIS:
        if (now >= g->next_step) {
            g->next_step = now + (uint64_t)tet_interval(&g->u.te);
            if (!tet_move(g, 0, 1)) tet_lock(g, now);
            changed = 1;
        }
        break;
    case GAME_PONG:
        for (int guard = 0; now >= g->next_step && guard < 8; guard++) { g->next_step += 2; pong_step(g); changed = 1; }
        if (now >= g->next_step) g->next_step = now + 2;
        break;
    default: break;
    }
    if (g->score > g->best[g->kind]) g->best[g->kind] = g->score;
    return changed;
}

const char *game_title(int kind) {
    switch (kind) { case GAME_SNAKE: return "Змейка"; case GAME_TETRIS: return "Тетрис"; case GAME_PONG: return "Понг"; case GAME_2048: return "2048"; }
    return "Игра";
}

const char *game_help(int kind) {
    switch (kind) {
    case GAME_SNAKE:  return "стрелки/WASD  P пауза  R заново";
    case GAME_TETRIS: return "стрелки  ПРОБЕЛ — сброс  P  R";
    case GAME_PONG:   return "мышь или стрелки вверх/вниз  до 7  P  R";
    case GAME_2048:   return "стрелки/WASD  собери 2048  R заново";
    }
    return "";
}

void game_size(int kind, int *w, int *h) {
    switch (kind) {
    case GAME_SNAKE:  *w = SNAKE_W * 16;  *h = 30 + SNAKE_H * 16 + 20; break;
    case GAME_TETRIS: *w = 20 + TET_W * 20 + 20 + 120; *h = 30 + TET_H * 20 + 30; break;
    case GAME_PONG:   *w = PONG_W; *h = 30 + PONG_H + 20; break;
    case GAME_2048:   *w = 4 * 72 + 5 * 8 + 16; *h = 30 + 4 * 72 + 5 * 8 + 28; break;
    default: *w = 320; *h = 240; break;
    }
}
