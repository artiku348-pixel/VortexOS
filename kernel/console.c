/* Consoles: every task writes to (and reads keys from) its console.
 * The kernel console `kcon` is the VGA text screen, or -- once graphics are
 * on -- a full-screen framebuffer terminal. Terminal windows have their own. */
#include "kernel.h"

struct console kcon;
static struct term fsterm;              /* full-screen terminal for kcon */
static int fsterm_ready, panicking;

static struct console *current(void)
{
    struct task *t = task_current();
    return t ? t->con : &kcon;
}

static void kcon_putc(char c)
{
    if (!gfx_active()) { vga_putc(c); return; }
    if (wm_running() && !panicking) return;     /* the desktop owns the screen */
    if (!fsterm_ready) console_takeover();
    term_putc(&fsterm, c);
}

/* (Re)start the full-screen terminal, e.g. after the desktop exits. */
void console_takeover(void)
{
    if (!gfx_active()) return;
    term_init(&fsterm, gfx_screen());
    fsterm_ready = 1;
}

/* Panic path: kernel output must reach the screen even over the desktop. */
void console_emergency(void)
{
    panicking = 1;
    console_takeover();
}

static void capture_putc(struct capture *cap, char c)
{
    if (cap->len == cap->cap) {
        u32 ncap = cap->cap ? cap->cap * 2 : 256;
        char *nbuf = kmalloc(ncap);
        if (!nbuf) return;
        if (cap->buf) { memcpy(nbuf, cap->buf, cap->len); kfree(cap->buf); }
        cap->buf = nbuf;
        cap->cap = ncap;
    }
    cap->buf[cap->len++] = c;
}

void kputc(char c)
{
    struct task *t = task_current();
    if (t && t->cap) { capture_putc(t->cap, c); return; }

    struct console *con = current();
    if (con->term) term_putc(con->term, c);
    else kcon_putc(c);
    if (c == '\n') serial_putc('\r');
    serial_putc(c);
}

void console_color(u8 fg, u8 bg)
{
    struct console *con = current();
    if (con->term) term_color(con->term, fg, bg);
    else if (!gfx_active()) vga_set_color(fg, bg);
    else if (fsterm_ready) term_color(&fsterm, fg, bg);
}

void console_clear(void)
{
    struct console *con = current();
    if (con->term) term_clear(con->term);
    else if (!gfx_active()) vga_clear();
    else if (!wm_running()) console_takeover();
}

void console_push_key(struct console *c, int key)
{
    if (key == 3 && c->fg_task) {       /* Ctrl+C goes to whoever waits in task_wait() */
        c->interrupted = 1;
        return;
    }
    u32 next = (c->head + 1) % sizeof(c->in);
    if (next == c->tail) return;
    c->in[c->head] = (u8)key;
    c->head = next;
}

/* Without the desktop, the kernel console reads the keyboard directly. */
static void pump(struct console *c)
{
    int key;
    if (c != &kcon || wm_running()) return;
    while ((key = keyboard_poll()) >= 0) console_push_key(c, key);
}

int console_peek(struct console *c)
{
    pump(c);
    return c->head == c->tail ? -1 : c->in[c->tail];
}

int console_getchar(void)
{
    struct console *c = current();
    while (console_peek(c) < 0) task_yield();
    int key = c->in[c->tail];
    c->tail = (c->tail + 1) % sizeof(c->in);
    return key;
}
