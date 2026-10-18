/* LimonOS 0.3 — точка входа ядра. */
#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "arch.h"
#include "console.h"
#include "cpuinfo.h"
#include "desktop.h"
#include "gfx.h"
#include "io.h"
#include "keyboard.h"
#include "mem.h"
#include "net.h"
#include "pci.h"
#include "mouse.h"
#include "ps2.h"
#include "sysinfo.h"
#include "timer.h"

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(2)

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER

static void halt(void) {
    for (;;) __asm__ volatile ("cli; hlt");
}

static void ok(const char *what) {
    console_set_color(0x7BD88F, 0x14181F);
    kprintf("[ OK ] ");
    console_set_color(0xD8DEE9, 0x14181F);
    kprintf("%s\n", what);
}

static void read_memmap(void) {
    struct limine_memmap_response *m = memmap_request.response;
    if (m == NULL) return;
    for (uint64_t i = 0; i < m->entry_count; i++) {
        struct limine_memmap_entry *e = m->entries[i];
        switch (e->type) {
        case LIMINE_MEMMAP_USABLE:
            g_sys.mem_usable += e->length; break;
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
            g_sys.mem_reclaim += e->length; break;
        case LIMINE_MEMMAP_KERNEL_AND_MODULES:
            g_sys.mem_reserved += e->length; break;
        default: break;   /* резерв, NVS, framebuffer — не считаем оперативной памятью */
        }
    }
    g_sys.mem_valid = 1;
}

void kmain(void) {
    if (!LIMINE_BASE_REVISION_SUPPORTED) halt();
    if (fb_request.response == NULL || fb_request.response->framebuffer_count < 1) halt();
    struct limine_framebuffer *fb = fb_request.response->framebuffers[0];
    if (!gfx_init(fb)) halt();

    console_init(0xD8DEE9, 0x14181F);
    console_set_color(0xF4D91A, 0x14181F);
    kprintf("LimonOS %s\n\n", LIMON_VERSION);
    console_set_color(0xD8DEE9, 0x14181F);

    g_sys.fb_w = (uint32_t)fb->width;
    g_sys.fb_h = (uint32_t)fb->height;
    g_sys.fb_bpp = fb->bpp;
    ok("Экран и консоль");

    gdt_init();         ok("GDT (таблица сегментов)");
    idt_init();         ok("IDT (таблица прерываний)");
    pic_init();         ok("PIC (контроллер прерываний)");
    timer_init();       ok("Таймер 100 Гц");
    cpuinfo_init(&g_sys.cpu);   ok("Процессор опознан");
    read_memmap();      ok("Карта памяти");
    ps2_init();         ok("Контроллер PS/2: клавиатура и мышь");
    kbd_init();
    cpu_sti();          /* дальше нужен таймер (ожидание ответов сети) */

    if (hhdm_request.response != NULL && memmap_request.response != NULL) {
        mem_init(hhdm_request.response->offset, memmap_request.response);
        heap_init(1024);                                    /* куча 4 МиБ */
        ok("Память: менеджер страниц и куча");
        pci_init();
        kprintf("       Среда: %s, устройств PCI: %d\n", cpuinfo_env_name(&g_sys.cpu), pci_count());
        if (net_init() == 0) {
            ok("Сетевая карта найдена");
            if (net_dhcp() == 0) {
                char ipb[16];
                net_ip_str(net_info()->ip, ipb);
                kprintf("       Адрес получен: %s\n", ipb);
            } else kprintf("       Адрес не получен (vine dhcp позже)\n");
        } else kprintf("       Сетевой карты Intel e1000 нет\n");
    } else kprintf("[ !! ] Загрузчик не передал карту памяти\n");

    desktop_init();
    desktop_run();
}
