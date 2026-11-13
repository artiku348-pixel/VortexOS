/* PS/2 keyboard (scancode set 1, US layout) -> IRQ1 -> ring buffer. */
#include "kernel.h"

static const char map_lower[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t', 'q','w','e','r','t','y','u','i','o','p','[',']', '\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ',
};
static const char map_upper[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t', 'Q','W','E','R','T','Y','U','I','O','P','{','}', '\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ',
};

#define BUF_SIZE 128
static volatile char buf[BUF_SIZE];
static volatile u32 head, tail;
static int shift, caps;

static void keyboard_irq(struct regs *r)
{
    (void)r;
    u8 sc = inb(0x60);

    if (sc == 0xE0) return;                         /* extended prefix: ignored */
    if (sc == 0x2A || sc == 0x36) { shift = 1; return; }
    if (sc == 0xAA || sc == 0xB6) { shift = 0; return; }
    if (sc == 0x3A) { caps = !caps; return; }
    if (sc & 0x80) return;                          /* key release */

    char c = map_lower[sc];
    if (c >= 'a' && c <= 'z') {
        if (shift ^ caps) c = map_upper[sc];
    } else if (shift) {
        c = map_upper[sc];
    }
    if (!c) return;

    u32 next = (head + 1) % BUF_SIZE;
    if (next != tail) { buf[head] = c; head = next; }
}

void keyboard_init(void)
{
    while (inb(0x64) & 1) inb(0x60);                /* drain stale bytes */
    irq_install(1, keyboard_irq);
}

char keyboard_getchar(void)
{
    while (head == tail) cpu_hlt();                 /* sleep until an IRQ */
    char c = buf[tail];
    tail = (tail + 1) % BUF_SIZE;
    return c;
}
