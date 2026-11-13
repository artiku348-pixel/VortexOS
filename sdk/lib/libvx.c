/* libvx: program entry point and a minimal C library for VortexOS apps. */
#include <vortex.h>

/* The kernel calls _start(argc, argv) as a normal C function. Programs may
 * define main as `int main(void)` or `int main(int argc, char **argv)`. */
int main(int argc, char **argv);
int _start(int argc, char **argv)
{
    vx_exit(main(argc, argv));
    return 0;
}

/* ---- memory and strings ------------------------------------------------ */

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++)) { }
    return dst;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return NULL;
}

int atoi(const char *s)
{
    int sign = 1, v = 0;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return sign * v;
}

/* ---- formatted output -------------------------------------------------- */

struct out { char *buf; size_t max, n; };

static void emit(struct out *o, char c)
{
    if (o->n + 1 < o->max) o->buf[o->n] = c;
    o->n++;
}

static void emit_num(struct out *o, unsigned v, unsigned base, int width, char padc, int neg, int left)
{
    char tmp[12];
    int n = 0;
    do {
        unsigned d = v % base;
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while (v);
    int len = n + neg;
    if (!left && padc == ' ') for (; width > len; width--) emit(o, ' ');
    if (neg) emit(o, '-');
    if (!left && padc == '0') for (; width > len; width--) emit(o, '0');
    while (n) emit(o, tmp[--n]);
    if (left) for (; width > len; width--) emit(o, ' ');
}

/* Supports %c %s %d %i %u %x %p %% with flags '-' and '0' and a width. */
int vsnprintf(char *buf, size_t max, const char *fmt, va_list ap)
{
    struct out o = { buf, max, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(&o, *fmt); continue; }
        int left = 0, width = 0;
        char padc = ' ';
        fmt++;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') padc = '0';
            else break;
        }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == 'l') fmt++;
        switch (*fmt) {
        case 'c': emit(&o, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            if (!left) for (; width > len; width--) emit(&o, ' ');
            while (*s) emit(&o, *s++);
            if (left) for (; width > len; width--) emit(&o, ' ');
            break;
        }
        case 'd': case 'i': {
            int v = va_arg(ap, int);
            emit_num(&o, v < 0 ? 0u - (unsigned)v : (unsigned)v, 10, width, padc, v < 0, left);
            break;
        }
        case 'u': emit_num(&o, va_arg(ap, unsigned), 10, width, padc, 0, left); break;
        case 'x': emit_num(&o, va_arg(ap, unsigned), 16, width, padc, 0, left); break;
        case 'p': emit(&o, '0'); emit(&o, 'x'); emit_num(&o, va_arg(ap, unsigned), 16, 8, '0', 0, 0); break;
        case '%': emit(&o, '%'); break;
        case '\0': fmt--; break;
        default: emit(&o, '%'); emit(&o, *fmt);
        }
    }
    if (max) buf[o.n < max ? o.n : max - 1] = 0;
    return (int)o.n;
}

int snprintf(char *buf, size_t max, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, max, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    vx_write(buf, (size_t)n);
    return n;
}

int putchar(int c)
{
    char ch = (char)c;
    vx_write(&ch, 1);
    return c;
}

int puts(const char *s)
{
    vx_write(s, strlen(s));
    putchar('\n');
    return 0;
}

int getchar(void) { return vx_getkey(); }

char *gets_s(char *buf, size_t max)
{
    size_t n = 0;
    for (;;) {
        int c = getchar();
        if (c == '\n') { putchar('\n'); break; }
        if (c == '\b') {
            if (n) { n--; putchar('\b'); }
        } else if (c >= ' ' && c < 127 && n + 1 < max) {
            buf[n++] = (char)c;
            putchar(c);
        }
    }
    buf[n] = 0;
    return buf;
}

/* ---- misc -------------------------------------------------------------- */

/* malloc: a first-fit free list over memory obtained with vx_sbrk(). */
struct block {
    size_t size;                        /* payload bytes */
    int    free;
    struct block *next;
    int    pad;
};
static struct block *heap_head, *heap_tail;

void *malloc(size_t size)
{
    if (!size) return NULL;
    size = (size + 15) & ~(size_t)15;

    struct block *b = heap_head;
    while (b && !(b->free && b->size >= size)) b = b->next;
    if (!b) {                           /* grow the heap */
        size_t chunk = size + sizeof(*b);
        if (chunk < 64 * 1024) chunk = 64 * 1024;
        b = vx_sbrk(chunk);
        if (b == (void *)-1) return NULL;
        b->size = chunk - sizeof(*b);
        b->next = NULL;
        if (heap_tail) heap_tail->next = b;
        else heap_head = b;
        heap_tail = b;
    }
    if (b->size >= size + sizeof(*b) + 16) {        /* split the remainder off */
        struct block *rest = (struct block *)((char *)(b + 1) + size);
        rest->size = b->size - size - sizeof(*b);
        rest->free = 1;
        rest->next = b->next;
        b->size = size;
        b->next = rest;
        if (heap_tail == b) heap_tail = rest;
    }
    b->free = 0;
    return b + 1;
}

void free(void *ptr)
{
    if (!ptr) return;
    ((struct block *)ptr - 1)->free = 1;
    for (struct block *b = heap_head; b; b = b->next)       /* merge free neighbours */
        while (b->free && b->next && b->next->free &&
               (char *)(b + 1) + b->size == (char *)b->next) {
            if (heap_tail == b->next) heap_tail = b;
            b->size += sizeof(*b) + b->next->size;
            b->next = b->next->next;
        }
}

static unsigned rand_state = 12345;
void srand(unsigned seed) { rand_state = seed; }
int  rand(void)
{
    rand_state = rand_state * 1103515245u + 12345u;
    return (int)((rand_state >> 16) & 0x7FFF);
}
