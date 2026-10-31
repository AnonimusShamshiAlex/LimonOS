/* Хост-стенд: собирает настоящий рабочий стол LimonOS на обычном компьютере, рисует в «фальшивый»
   экран и сохраняет кадры в PPM. Так можно проверить внешний вид без QEMU/VirtualBox. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "desktop.h"
#include "gfx.h"
#include "keyboard.h"
#include "mouse.h"
#include "timer.h"
#include "games.h"

/* ---- заглушки для «железа» ---- */
void *phys_to_virt(uint64_t p) { return (void *)p; }

static uint32_t *fb;
static void save(const char *name, int w, int h) {
    FILE *f = fopen(name, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { fputc((fb[i] >> 16) & 255, f); fputc((fb[i] >> 8) & 255, f); fputc(fb[i] & 255, f); }
    fclose(f);
}

static void key(uint32_t cp) { struct key_event e = { cp, 0 }; desktop_key(&e); }
static void run_ticks(int n) { for (int i = 0; i < n; i++) { timer_ticks++; desktop_tick(); desktop_frame(); } }

int main(int argc, char **argv) {
    int W = argc > 1 ? atoi(argv[1]) : 1024, H = argc > 2 ? atoi(argv[2]) : 768;
    fb = calloc((size_t)W * H, 4);
    struct limine_framebuffer lf = { 0 };
    lf.address = fb; lf.width = W; lf.height = H; lf.pitch = W * 4; lf.bpp = 32;
    lf.red_mask_size = 8; lf.red_mask_shift = 16; lf.green_mask_size = 8; lf.green_mask_shift = 8; lf.blue_mask_size = 8; lf.blue_mask_shift = 0;
    gfx_init(&lf);
    desktop_init();
    desktop_frame(); gfx_present();
    save("/tmp/shot_desktop.ppm", W, H);

    desktop_open_game(GAME_SNAKE);
    key(KEY_RIGHT); run_ticks(400);
    desktop_frame(); gfx_present(); save("/tmp/shot_snake.ppm", W, H);

    desktop_open_game(GAME_TETRIS);
    key(KEY_LEFT); for (int i = 0; i < 6; i++) { key(' '); run_ticks(10); }
    key(KEY_UP); run_ticks(30);
    desktop_frame(); gfx_present(); save("/tmp/shot_tetris.ppm", W, H);

    desktop_open_game(GAME_PONG); key(KEY_DOWN); run_ticks(200);
    desktop_open_game(GAME_2048); key(KEY_LEFT); key(KEY_DOWN); key(KEY_LEFT); run_ticks(5);
    desktop_frame(); gfx_present(); save("/tmp/shot_all.ppm", W, H);
    for (int t = 1; t < desktop_theme_count(); t++) {
        desktop_set_theme(t); desktop_frame(); gfx_present();
        char n[64]; snprintf(n, sizeof n, "/tmp/shot_theme%d.ppm", t); save(n, W, H);
    }
    puts("shots ok");
    return 0;
}
