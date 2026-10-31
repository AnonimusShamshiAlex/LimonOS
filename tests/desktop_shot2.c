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
#include "vfs.h"
void *phys_to_virt(uint64_t p) { return (void *)p; }
static uint32_t *fb;
static void save(const char *name, int w, int h) {
    FILE *f = fopen(name, "wb"); fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { fputc((fb[i]>>16)&255,f); fputc((fb[i]>>8)&255,f); fputc(fb[i]&255,f); }
    fclose(f);
}
static void key(uint32_t cp) { struct key_event e = { cp, 0 }; desktop_key(&e); }
static void click(int x, int y) { struct mouse_event e = { x, y, 1, 0 }; desktop_mouse(&e); e.buttons = 0; desktop_mouse(&e); }
static void dclick(int x, int y) { click(x,y); run_tick: ; for(int i=0;i<30;i++){timer_ticks++; desktop_tick();} click(x,y); }
static void run_ticks(int n) { for (int i = 0; i < n; i++) { timer_ticks++; desktop_tick(); desktop_frame(); } }

int main(int argc, char **argv) {
    int W = argc>1?atoi(argv[1]):1024, H = argc>2?atoi(argv[2]):768;
    fb = calloc((size_t)W*H,4);
    struct limine_framebuffer lf = {0};
    lf.address=fb; lf.width=W; lf.height=H; lf.pitch=W*4; lf.bpp=32;
    lf.red_mask_size=8; lf.red_mask_shift=16; lf.green_mask_size=8; lf.green_mask_shift=8; lf.blue_mask_size=8; lf.blue_mask_shift=0;
    gfx_init(&lf);
    vfs_init();
    desktop_init();

    desktop_open_files(-1);
    run_ticks(5);
    desktop_frame(); gfx_present(); save("/tmp/shot_files_root.ppm", W, H);

    /* войти в Downloads двойным кликом (окно начинается ~ дефолт позиция, найдём через desktop_open_files индекс - используем прямой API) */
    desktop_open_files(vfs_downloads);
    run_ticks(5);
    desktop_frame(); gfx_present(); save("/tmp/shot_files_dl.ppm", W, H);

    key('n'); 
    key('T'); key('e'); key('s'); key('t');
    key('\n');
    run_ticks(5);
    desktop_frame(); gfx_present(); save("/tmp/shot_files_mkdir.ppm", W, H);

    desktop_open_taskmgr();
    desktop_open_terminal();
    desktop_open_calculator();
    desktop_open_taskmgr();   /* повторный вызов — просто фокусирует уже открытый диспетчер */
    run_ticks(5);
    desktop_frame(); gfx_present(); save("/tmp/shot_taskmgr.ppm", W, H);

    puts("shots2 ok");
    return 0;
}
