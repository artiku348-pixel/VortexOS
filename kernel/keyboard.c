/* PS/2 keyboard (scancode set 1, US layout) -> IRQ1 -> ring buffer of
 * VX_KEY_* codes. */
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
static volatile u8 buf[BUF_SIZE];
static volatile u32 head, tail;
static int shift, caps, ctrl, extended;

static void put(int key)
{
    u32 next = (head + 1) % BUF_SIZE;
    if (next != tail) { buf[head] = (u8)key; head = next; }
}

static int extended_key(u8 sc)
{
    switch (sc) {
    case 0x48: return VX_KEY_UP;
    case 0x50: return VX_KEY_DOWN;
    case 0x4B: return VX_KEY_LEFT;
    case 0x4D: return VX_KEY_RIGHT;
    case 0x47: return VX_KEY_HOME;
    case 0x4F: return VX_KEY_END;
    case 0x49: return VX_KEY_PGUP;
    case 0x51: return VX_KEY_PGDN;
    case 0x53: return VX_KEY_DELETE;
    case 0x1C: return '\n';             /* keypad Enter */
    }
    return 0;
}

static void keyboard_irq(struct regs *r)
{
    (void)r;
    u8 sc = inb(0x60);

    if (sc == 0xE0) { extended = 1; return; }
    if (extended) {
        extended = 0;
        if (sc == 0x1D) ctrl = 1;                   /* right Ctrl */
        else if (sc == 0x9D) ctrl = 0;
        else if (!(sc & 0x80)) {
            int key = extended_key(sc);
            if (key) put(key);
        }
        return;
    }
    if (sc == 0x2A || sc == 0x36) { shift = 1; return; }
    if (sc == 0xAA || sc == 0xB6) { shift = 0; return; }
    if (sc == 0x1D) { ctrl = 1; return; }
    if (sc == 0x9D) { ctrl = 0; return; }
    if (sc == 0x3A) { caps = !caps; return; }
    if (sc & 0x80) return;                          /* key release */

    char c = map_lower[sc];
    if (c >= 'a' && c <= 'z') {
        if (ctrl) c = (char)(c - 'a' + 1);          /* Ctrl+C = 3, ... */
        else if (shift ^ caps) c = map_upper[sc];
    } else if (shift) {
        c = map_upper[sc];
    }
    if (c) put(c);
}

void keyboard_init(void)
{
    while (inb(0x64) & 1) inb(0x60);                /* drain stale bytes */
    irq_install(1, keyboard_irq);
}

int keyboard_poll(void)
{
    if (head == tail) return -1;
    int key = buf[tail];
    tail = (tail + 1) % BUF_SIZE;
    return key;
}
