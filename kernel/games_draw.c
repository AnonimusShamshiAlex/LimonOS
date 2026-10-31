/* Рисование игр в стиле LimonOS: толстые контуры, неон, жёсткие тени. */
#include "games.h"
#include "gfx.h"
#include "font.h"
#include "format.h"
#include "timer.h"

static const uint32_t PIECE_COL[8] = { 0x000000, 0x00E5FF, 0xFFE600, 0xB84DFF, 0x39FF14, 0xFF2D55, 0x2F7BFF, 0xFF8A00 };

static void text_c(int cx, int y, const char *s, uint32_t col, int scale) {
    gfx_draw_text(cx - gfx_text_width(s, scale) / 2, y, s, col, scale);
}

/* кирпичик с «объёмом»: светлая грань сверху-слева, тёмная снизу-справа */
static void block(int x, int y, int sz, uint32_t col) {
    gfx_fill_rect(x, y, sz, sz, 0x000000);
    gfx_fill_rect(x + 1, y + 1, sz - 2, sz - 2, col);
    gfx_fill_rect(x + 1, y + 1, sz - 2, 2, gfx_mix(col, 0xFFFFFF, 110));
    gfx_fill_rect(x + 1, y + 1, 2, sz - 2, gfx_mix(col, 0xFFFFFF, 70));
    gfx_fill_rect(x + 1, y + sz - 3, sz - 2, 2, gfx_mix(col, 0x000000, 110));
}

static void header(const struct game *g, int x, int y, int w, const struct game_pal *p, const char *extra) {
    gfx_fill_rect(x, y, w, 26, p->panel);
    gfx_fill_rect(x, y + 25, w, 1, p->accent);
    char b[64];
    ksnprintf(b, sizeof b, "СЧЁТ %d", g->score);
    gfx_draw_text(x + 8, y + 5, b, p->accent, 1);
    ksnprintf(b, sizeof b, "РЕКОРД %d", g->best[g->kind]);
    gfx_draw_text(x + 8 + 12 * FONT_W, y + 5, b, p->fg, 1);
    if (extra) gfx_draw_text(x + w - gfx_text_width(extra, 1) - 8, y + 5, extra, p->accent2, 1);
}

static void footer(const struct game *g, int x, int y, int w, const struct game_pal *p) {
    gfx_fill_rect(x, y, w, 20, p->panel);
    const char *h = game_help(g->kind);
    gfx_draw_text(x + (w - gfx_text_width(h, 1)) / 2, y + 3, h, p->dim, 1);
}

static void overlay(const struct game *g, int x, int y, int w, int h, const struct game_pal *p) {
    if (!g->over && !g->paused) return;
    gfx_blend_rect(x, y, w, h, 0x000000, 150);
    int bw = w - 40 > 260 ? 260 : w - 40, bh = 64;
    int bx = x + (w - bw) / 2, by = y + (h - bh) / 2;
    gfx_fill_rect(bx + 5, by + 5, bw, bh, p->accent2);                 /* жёсткая тень */
    gfx_fill_rect(bx, by, bw, bh, 0x000000);
    gfx_draw_rect(bx, by, bw, bh, p->accent);
    gfx_draw_rect(bx + 1, by + 1, bw - 2, bh - 2, p->accent);
    if (g->over) {
        int win = (g->kind == GAME_PONG && g->u.po.ps >= 7) || (g->kind == GAME_2048 && g->u.g.won)
               || (g->kind == GAME_SNAKE && g->u.sn.len >= SNAKE_MAX);
        text_c(bx + bw / 2, by + 10, win ? "ПОБЕДА!" : "КОНЕЦ ИГРЫ", win ? p->good : p->bad, 2);
        text_c(bx + bw / 2, by + 42, "ENTER или R — ещё раз", p->fg, 1);
    } else {
        text_c(bx + bw / 2, by + 10, g->fresh ? "ГОТОВ?" : "ПАУЗА", p->accent, 2);
        text_c(bx + bw / 2, by + 42, g->fresh ? "любая клавиша — старт" : "любая клавиша — дальше", p->fg, 1);
    }
}

/* ---- Змейка ---- */
static void draw_snake(const struct game *g, int x, int y, const struct game_pal *p) {
    const struct snake_st *s = &g->u.sn;
    int w = SNAKE_W * 16, top = y + 30;
    header(g, x, y, w, p, NULL);
    gfx_fill_rect(x, top, w, SNAKE_H * 16, p->bg);
    for (int j = 0; j < SNAKE_H; j++) for (int i = 0; i < SNAKE_W; i++)
        gfx_fill_rect(x + i * 16 + 7, top + j * 16 + 7, 2, 2, p->panel);          /* точечная сетка */
    uint64_t t = timer_ticks;
    if (s->fx >= 0) {
        int fx = x + s->fx * 16, fy = top + s->fy * 16;
        int pulse = (int)((t / 15) & 1);
        gfx_fill_rect(fx + 2 - pulse, fy + 2 - pulse, 12 + 2 * pulse, 12 + 2 * pulse, p->accent2);
        gfx_fill_rect(fx + 6, fy + 6, 4, 4, 0xFFFFFF);
    }
    if (s->bonus_x >= 0 && ((t / 10) & 1)) block(x + s->bonus_x * 16, top + s->bonus_y * 16, 16, 0xFFE600);
    for (int i = s->len - 1; i >= 0; i--) {
        uint32_t col = i == 0 ? 0xFFFFFF : gfx_mix(p->accent, p->accent2, (uint32_t)(i * 255 / (s->len + 4)) );
        block(x + s->x[i] * 16, top + s->y[i] * 16, 16, col);
    }
    gfx_fill_rect(x + s->x[0] * 16 + 4, top + s->y[0] * 16 + 5, 3, 3, 0x000000);   /* глаза */
    gfx_fill_rect(x + s->x[0] * 16 + 9, top + s->y[0] * 16 + 5, 3, 3, 0x000000);
    gfx_draw_rect(x, top, w, SNAKE_H * 16, p->accent);
    footer(g, x, top + SNAKE_H * 16, w, p);
    overlay(g, x, top, w, SNAKE_H * 16, p);
}

/* ---- Тетрис ---- */
static void draw_tetris(const struct game *g, int x, int y, const struct game_pal *p) {
    const struct tetris_st *t = &g->u.te;
    int bx = x + 20, by = y + 30, W = 20 + TET_W * 20 + 20 + 120;
    header(g, x, y, W, p, NULL);
    gfx_fill_rect(x, by, W, TET_H * 20 + 30, p->bg);
    gfx_fill_rect(bx - 3, by - 3, TET_W * 20 + 6, TET_H * 20 + 6, p->accent);
    gfx_fill_rect(bx, by, TET_W * 20, TET_H * 20, 0x05060A);
    for (int j = 0; j < TET_H; j++) for (int i = 0; i < TET_W; i++) {
        if (t->board[j][i]) block(bx + i * 20, by + j * 20, 20, PIECE_COL[t->board[j][i]]);
        else gfx_fill_rect(bx + i * 20 + 9, by + j * 20 + 9, 2, 2, p->panel);
    }
    if (!g->over) {
        int c[4][2];
        tetris_piece_cells(t->piece, t->rot, c);
        int gy = tetris_ghost_y(g);
        for (int k = 0; k < 4; k++) {                                            /* призрак: куда упадёт */
            int cx = bx + (t->px + c[k][0]) * 20, cy = by + (gy + c[k][1]) * 20;
            if (gy + c[k][1] >= 0) gfx_draw_rect(cx + 1, cy + 1, 18, 18, PIECE_COL[t->piece + 1]);
        }
        for (int k = 0; k < 4; k++) {
            int yy = t->py + c[k][1];
            if (yy >= 0) block(bx + (t->px + c[k][0]) * 20, by + yy * 20, 20, PIECE_COL[t->piece + 1]);
        }
    }
    int sx = bx + TET_W * 20 + 20;
    gfx_draw_text(sx, by, "ДАЛЬШЕ", p->accent, 1);
    gfx_fill_rect(sx, by + 18, 100, 70, 0x05060A);
    gfx_draw_rect(sx, by + 18, 100, 70, p->accent2);
    int c[4][2];
    tetris_piece_cells(t->next, 0, c);
    for (int k = 0; k < 4; k++) block(sx + 18 + c[k][0] * 16, by + 28 + c[k][1] * 16, 16, PIECE_COL[t->next + 1]);
    char b[32];
    gfx_draw_text(sx, by + 110, "СТРОКИ", p->accent, 1);
    ksnprintf(b, sizeof b, "%d", t->lines);   gfx_draw_text(sx, by + 128, b, p->fg, 2);
    gfx_draw_text(sx, by + 164, "УРОВЕНЬ", p->accent, 1);
    ksnprintf(b, sizeof b, "%d", t->level + 1); gfx_draw_text(sx, by + 182, b, p->fg, 2);
    footer(g, x, by + TET_H * 20 + 10, W, p);
    overlay(g, bx, by, TET_W * 20, TET_H * 20, p);
}

/* ---- Понг ---- */
static void draw_pong(const struct game *g, int x, int y, const struct game_pal *p) {
    const struct pong_st *o = &g->u.po;
    int top = y + 30;
    header(g, x, y, PONG_W, p, NULL);
    gfx_fill_rect(x, top, PONG_W, PONG_H, 0x05060A);
    for (int j = 0; j < PONG_H; j += 16) gfx_fill_rect(x + PONG_W / 2 - 2, top + j, 4, 8, p->panel);
    char b[8];
    ksnprintf(b, sizeof b, "%d", o->ps); gfx_draw_text(x + PONG_W / 2 - 60, top + 10, b, p->accent, 3);
    ksnprintf(b, sizeof b, "%d", o->as); gfx_draw_text(x + PONG_W / 2 + 36, top + 10, b, p->accent2, 3);
    gfx_fill_rect(x + 10, top + o->py, 10, PONG_PADH, p->accent);
    gfx_fill_rect(x + PONG_W - 20, top + o->ay, 10, PONG_PADH, p->accent2);
    if (o->serve_wait == 0 || (timer_ticks / 8) & 1)
        gfx_fill_rect(x + o->ball_x / 256 - 4, top + o->ball_y / 256 - 4, 8, 8, 0xFFFFFF);
    gfx_draw_rect(x, top, PONG_W, PONG_H, p->accent);
    footer(g, x, top + PONG_H, PONG_W, p);
    overlay(g, x, top, PONG_W, PONG_H, p);
}

/* ---- 2048 ---- */
static uint32_t tile_col(int v) {
    switch (v) {
    case 2: return 0x2B2F3A; case 4: return 0x3A3F52; case 8: return 0xFF8A00; case 16: return 0xFF5A1F;
    case 32: return 0xFF2D55; case 64: return 0xD6006F; case 128: return 0xB84DFF; case 256: return 0x6C4DFF;
    case 512: return 0x2F7BFF; case 1024: return 0x00C2FF; case 2048: return 0xC6FF00; default: return 0xFFFFFF;
    }
}

static void draw_2048(const struct game *g, int x, int y, const struct game_pal *p) {
    int W = 4 * 72 + 5 * 8 + 16, top = y + 30, bd = 4 * 72 + 5 * 8;
    header(g, x, y, W, p, NULL);
    gfx_fill_rect(x, top, W, bd + 28, p->bg);
    int ox = x + 8, oy = top + 4;
    gfx_fill_rect(ox, oy, bd, bd, 0x000000);
    gfx_draw_rect(ox - 1, oy - 1, bd + 2, bd + 2, p->accent);
    for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {
        int v = g->u.g.cell[j][i];
        int tx = ox + 8 + i * 80, ty = oy + 8 + j * 80;
        gfx_fill_rect(tx, ty, 72, 72, v ? tile_col(v) : 0x14171F);
        if (v) {
            char b[8]; ksnprintf(b, sizeof b, "%d", v);
            int sc = v < 100 ? 3 : v < 1000 ? 2 : 2;
            uint32_t fg = (v == 2048) ? 0x000000 : 0xFFFFFF;
            gfx_draw_text(tx + (72 - gfx_text_width(b, sc)) / 2, ty + (72 - FONT_H * sc) / 2, b, fg, sc);
        }
    }
    footer(g, x, top + bd + 8, W, p);
    overlay(g, ox, oy, bd, bd, p);
}

void game_draw(const struct game *g, int x, int y, const struct game_pal *p) {
    switch (g->kind) {
    case GAME_SNAKE:  draw_snake(g, x, y, p); break;
    case GAME_TETRIS: draw_tetris(g, x, y, p); break;
    case GAME_PONG:   draw_pong(g, x, y, p); break;
    case GAME_2048:   draw_2048(g, x, y, p); break;
    default: break;
    }
}
