/* CMOS real-time clock. */
#include "kernel.h"

static u8 cmos(u8 reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static int bcd(u8 v) { return (v >> 4) * 10 + (v & 0x0F); }

void rtc_read(struct vx_time *t)
{
    for (int i = 0; i < 100000 && (cmos(0x0A) & 0x80); i++) { }    /* update in progress */

    u8 sec = cmos(0x00), min = cmos(0x02), hour = cmos(0x04);
    u8 day = cmos(0x07), mon = cmos(0x08), year = cmos(0x09);
    u8 status_b = cmos(0x0B);
    int pm = hour & 0x80;
    hour &= 0x7F;

    if (status_b & 0x04) {              /* binary mode */
        t->second = sec; t->minute = min; t->hour = hour;
        t->day = day; t->month = mon; t->year = year;
    } else {
        t->second = bcd(sec); t->minute = bcd(min); t->hour = bcd(hour);
        t->day = bcd(day); t->month = bcd(mon); t->year = bcd(year);
    }
    if (!(status_b & 0x02)) {           /* 12-hour mode */
        t->hour %= 12;
        if (pm) t->hour += 12;
    }
    t->year += 2000;
}
