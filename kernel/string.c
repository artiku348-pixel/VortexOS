#include "kernel.h"

void *memset(void *dst, int c, size_t n)
{
    u8 *d = dst;
    while (n--) *d++ = (u8)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
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
    return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (u8)*a - (u8)*b : 0;
}

int atoi(const char *s)
{
    int sign = 1, v = 0;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return sign * v;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++)) { }
    return dst;
}

/* Bounded copy that always terminates dst (max = size of dst). */
void strlcpy(char *dst, const char *src, size_t max)
{
    if (!max) return;
    while (--max && *src) *dst++ = *src++;
    *dst = 0;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return NULL;
}

char *strrchr(const char *s, int c)
{
    const char *r = NULL;
    for (; *s; s++) if (*s == (char)c) r = s;
    return (char *)r;
}

void memset32(u32 *dst, u32 value, size_t count)
{
    __asm__ volatile("cld; rep stosl" : "+D"(dst), "+c"(count) : "a"(value) : "memory");
}

void memcpy32(u32 *dst, const u32 *src, size_t count)
{
    __asm__ volatile("cld; rep movsl" : "+D"(dst), "+S"(src), "+c"(count) : : "memory");
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
