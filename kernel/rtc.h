#ifndef LIMON_RTC_H
#define LIMON_RTC_H
struct rtc_time { int year, month, day, hour, min, sec; };
void rtc_read(struct rtc_time *t);   /* часы реального времени (CMOS) */
#endif
