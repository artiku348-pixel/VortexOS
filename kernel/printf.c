#include "kernel.h"

void kputs(const char *s)
{
    while (*s) kputc(*s++);
}

static void put_num(u32 v, u32 base, int width, char pad, int neg)
{
    char buf[12];
    int n = 0;
    do {
        u32 d = v % base;
        buf[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while (v);
    if (neg && pad == '0') { kputc('-'); width--; }
    else if (neg) buf[n++] = '-';
    while (width-- > n) kputc(pad);
    while (n) kputc(buf[--n]);
}

/* Supports %c %s %d %u %x %p %% with optional zero-pad / width (e.g. %08x). */
void kprintf(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { kputc(*fmt); continue; }
        char pad = ' ';
        int width = 0;
        fmt++;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        switch (*fmt) {
        case 'c': kputc((char)__builtin_va_arg(ap, int)); break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            for (int n = (int)strlen(s); n < width; n++) kputc(' ');
            kputs(s);
            break;
        }
        case 'd': {
            i32 v = __builtin_va_arg(ap, i32);
            put_num(v < 0 ? 0u - (u32)v : (u32)v, 10, width, pad, v < 0);
            break;
        }
        case 'u': put_num(__builtin_va_arg(ap, u32), 10, width, pad, 0); break;
        case 'x': put_num(__builtin_va_arg(ap, u32), 16, width, pad, 0); break;
        case 'p': kputs("0x"); put_num(__builtin_va_arg(ap, u32), 16, 8, '0', 0); break;
        case '%': kputc('%'); break;
        case '\0': fmt--; break;
        default:  kputc('%'); kputc(*fmt);
        }
    }
    __builtin_va_end(ap);
}

void panic(const char *msg)
{
    struct task *t = task_current();
    cpu_cli();
    if (t) { t->cap = NULL; t->con = &kcon; }
    console_emergency();
    console_color(VX_WHITE, VX_RED);
    kprintf("\n*** KERNEL PANIC: %s ***\nSystem halted.\n", msg);
    for (;;) cpu_hlt();
}
