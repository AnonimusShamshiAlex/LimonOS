#include "rtc.h"
#include "io.h"
#include "kstring.h"

static uint8_t cmos(uint8_t reg) { outb(0x70, reg); return inb(0x71); }

static void read_raw(uint8_t r[6]) {
    for (int i = 0; i < 100000 && (cmos(0x0A) & 0x80); i++) { }   /* ждём конца обновления */
    r[0] = cmos(0x00); r[1] = cmos(0x02); r[2] = cmos(0x04);
    r[3] = cmos(0x07); r[4] = cmos(0x08); r[5] = cmos(0x09);
}

static int bcd(int v) { return (v & 0x0F) + (v >> 4) * 10; }

void rtc_read(struct rtc_time *t) {
    uint8_t a[6], b[6];
    int tries = 0;
    do { read_raw(a); read_raw(b); } while (memcmp(a, b, 6) != 0 && ++tries < 5);
    uint8_t rb = cmos(0x0B);
    int sec = a[0], min = a[1], hour = a[2], day = a[3], mon = a[4], year = a[5];
    int pm = hour & 0x80;
    hour &= 0x7F;
    if (!(rb & 4)) { sec = bcd(sec); min = bcd(min); hour = bcd(hour); day = bcd(day); mon = bcd(mon); year = bcd(year); }
    if (!(rb & 2)) { hour = hour % 12; if (pm) hour += 12; }
    t->sec = sec; t->min = min; t->hour = hour; t->day = day; t->month = mon; t->year = 2000 + year;
}
