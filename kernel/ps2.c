#include "ps2.h"
#include "io.h"

static int wait_write(void) { for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 2)) return 1; return 0; }
static int wait_read(void)  { for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) return 1; return 0; }
static void cmd(uint8_t c)  { wait_write(); outb(0x64, c); }
static void data(uint8_t d) { wait_write(); outb(0x60, d); }
static int  read(void)      { return wait_read() ? inb(0x60) : -1; }
static int  mouse_send(uint8_t b) { cmd(0xD4); data(b); return read(); }   /* ждём 0xFA (ACK) */
static void flush(void)     { for (int i = 0; i < 64 && (inb(0x64) & 1); i++) inb(0x60); }

void ps2_init(void) {
    flush();
    cmd(0xAE);                         /* включить клавиатуру */
    cmd(0xA8);                         /* включить порт мыши */
    cmd(0x20);                         /* прочитать байт конфигурации */
    int cfg = read();
    if (cfg < 0) cfg = 0x47;
    cfg |= 0x03;                       /* прерывания IRQ1 и IRQ12 */
    cfg &= ~0x20;                      /* тактирование мыши включено */
    cmd(0x60); data((uint8_t)cfg);
    mouse_send(0xF6);                  /* параметры по умолчанию */
    mouse_send(0xF4);                  /* начать передачу данных */
    flush();
}
