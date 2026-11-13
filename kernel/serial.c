/* COM1 output, handy with `qemu -serial stdio`. */
#include "kernel.h"

#define COM1 0x3F8
static int present;

void serial_init(void)
{
    outb(COM1 + 1, 0x00);               /* no interrupts */
    outb(COM1 + 3, 0x80);               /* DLAB on */
    outb(COM1 + 0, 0x03);               /* 38400 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);               /* 8N1 */
    outb(COM1 + 2, 0xC7);               /* FIFO */
    outb(COM1 + 4, 0x0B);
    present = inb(COM1 + 5) != 0xFF;    /* floating bus => no UART */
}

void serial_putc(char c)
{
    if (!present) return;
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) { }
    outb(COM1, (u8)c);
}
