/* PIT channel 0 -> IRQ0 at TIMER_HZ. */
#include "kernel.h"

static volatile u32 ticks;

static void timer_irq(struct regs *r)
{
    (void)r;
    ticks++;
}

void timer_init(void)
{
    u32 divisor = 1193182 / TIMER_HZ;
    outb(0x43, 0x36);                   /* channel 0, lo/hi, square wave */
    outb(0x40, (u8)divisor);
    outb(0x40, (u8)(divisor >> 8));
    irq_install(0, timer_irq);
}

u32 timer_ticks(void) { return ticks; }

void sleep_ms(u32 ms)
{
    u32 wait = (ms * TIMER_HZ + 999) / 1000;
    u32 start = ticks;
    while (ticks - start < wait) task_yield();     /* the idle task halts */
}
