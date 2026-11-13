/* PS/2 mouse on the auxiliary port of the 8042 controller (IRQ12). */
#include "kernel.h"

#define BUF_SIZE 64
static struct mouse_packet buf[BUF_SIZE];
static volatile u32 head, tail;
static u8 bytes[3];
static int phase;

static int wait_write(void)
{
    for (int i = 0; i < 100000; i++)
        if (!(inb(0x64) & 2)) return 1;
    return 0;
}

static int wait_read(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(0x64) & 1) return 1;
    return 0;
}

static void mouse_cmd(u8 cmd)
{
    wait_write(); outb(0x64, 0xD4);     /* next byte goes to the mouse */
    wait_write(); outb(0x60, cmd);
    if (wait_read()) inb(0x60);         /* ACK */
}

static void mouse_irq(struct regs *r)
{
    (void)r;
    u8 b = inb(0x60);

    if (phase == 0 && !(b & 0x08)) return;          /* resynchronise */
    bytes[phase++] = b;
    if (phase < 3) return;
    phase = 0;
    if (bytes[0] & 0xC0) return;                    /* overflow: drop */

    u32 next = (head + 1) % BUF_SIZE;
    if (next == tail) return;
    buf[head].dx = (int)bytes[1] - ((bytes[0] & 0x10) ? 256 : 0);
    buf[head].dy = -((int)bytes[2] - ((bytes[0] & 0x20) ? 256 : 0));
    buf[head].buttons = bytes[0] & 7;
    head = next;
}

void mouse_init(void)
{
    wait_write(); outb(0x64, 0xA8);     /* enable the auxiliary device */
    wait_write(); outb(0x64, 0x20);     /* read the controller config byte */
    u8 cfg = wait_read() ? inb(0x60) : 0x47;
    cfg = (u8)((cfg | 0x02) & ~0x20);   /* IRQ12 on, mouse clock on */
    wait_write(); outb(0x64, 0x60);
    wait_write(); outb(0x60, cfg);

    mouse_cmd(0xF6);                    /* defaults */
    mouse_cmd(0xF4);                    /* start streaming packets */
    irq_install(12, mouse_irq);
}

int mouse_poll(struct mouse_packet *p)
{
    if (head == tail) return 0;
    *p = buf[tail];
    tail = (tail + 1) % BUF_SIZE;
    return 1;
}
