/* VortexOS application SDK.
 *
 *   #include <vortex.h>
 *   int main(int argc, char **argv) { printf("Hello!\n"); return 0; }
 *
 * Programs are freestanding, position-independent 64-bit ELF executables linked
 * against libvx (sdk/lib). Build flags live in the top-level Makefile; the
 * quickest start is `tools/new-app.sh <name> [gui]`.
 */
#ifndef VORTEX_H
#define VORTEX_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <vortex/abi.h>

/* ---- raw system call --------------------------------------------------- */
/* Number in rax, arguments in rdi, rsi, rdx, r10, r8; the result comes back
 * in rax. All other registers are preserved by the kernel. */
static inline long vx_syscall(long nr, long a, long b, long c, long d, long e)
{
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "memory", "cc");
    return ret;
}
#define VX_CALL0(n)             vx_syscall(n, 0, 0, 0, 0, 0)
#define VX_CALL1(n, a)          vx_syscall(n, (long)(a), 0, 0, 0, 0)
#define VX_CALL2(n, a, b)       vx_syscall(n, (long)(a), (long)(b), 0, 0, 0)
#define VX_CALL3(n, a, b, c)    vx_syscall(n, (long)(a), (long)(b), (long)(c), 0, 0)
#define VX_CALL4(n, a, b, c, d) vx_syscall(n, (long)(a), (long)(b), (long)(c), (long)(d), 0)

/* ---- process and console ----------------------------------------------- */
static inline void vx_exit(int code)                { VX_CALL1(VX_SYS_EXIT, code); for (;;) { } }
static inline int  vx_write(const char *s, size_t n) { return VX_CALL2(VX_SYS_WRITE, s, n); }
static inline int  vx_getkey(void)                  { return VX_CALL0(VX_SYS_GETKEY); }  /* blocks */
static inline void vx_sleep(unsigned ms)            { VX_CALL1(VX_SYS_SLEEP, ms); }
static inline unsigned vx_ticks(void)               { return (unsigned)VX_CALL0(VX_SYS_TICKS); }
static inline void vx_time(struct vx_time *t)       { VX_CALL1(VX_SYS_TIME, t); }
static inline void vx_yield(void)                   { VX_CALL0(VX_SYS_YIELD); }
static inline void vx_color(int fg, int bg)         { VX_CALL2(VX_SYS_COLOR, fg, bg); }
static inline void vx_clear(void)                   { VX_CALL0(VX_SYS_CLEAR); }
/* Grow the heap by `bytes`; returns the old end of the heap or (void *)-1. */
static inline void *vx_sbrk(size_t bytes)           { return (void *)VX_CALL1(VX_SYS_SBRK, bytes); }
/* Start a program found via $PATH; returns its task id or -1. */
static inline int  vx_spawn(const char *name, int argc, char **argv)
                                                    { return VX_CALL3(VX_SYS_SPAWN, name, argc, argv); }
static inline int  vx_wait(int task)                { return VX_CALL1(VX_SYS_WAIT, task); }
/* Returns the value length, or -1 if the variable is not set. */
static inline int  vx_getenv(const char *name, char *buf, size_t max)
                                                    { return VX_CALL3(VX_SYS_GETENV, name, buf, max); }
static inline int  vx_setenv(const char *name, const char *value)
                                                    { return VX_CALL2(VX_SYS_SETENV, name, value); }
static inline int  vx_getcwd(char *buf, size_t max) { return VX_CALL2(VX_SYS_GETCWD, buf, max); }
static inline int  vx_chdir(const char *path)       { return VX_CALL1(VX_SYS_CHDIR, path); }

/* ---- files ------------------------------------------------------------- */
/* Read up to `max` bytes starting at `offset`; returns bytes read or -1. */
static inline int vx_read_file(const char *path, void *buf, size_t max, size_t offset)
                                                    { return VX_CALL4(VX_SYS_FS_READ, path, buf, max, offset); }
/* Replace (append = 0) or extend (append = 1) a file, creating it if needed. */
static inline int vx_write_file(const char *path, const void *buf, size_t len, int append)
                                                    { return VX_CALL4(VX_SYS_FS_WRITE, path, buf, len, append); }
static inline int vx_stat(const char *path, struct vx_stat *st)
                                                    { return VX_CALL2(VX_SYS_FS_STAT, path, st); }
/* Entry number `index` of a directory: 1 = file, 2 = directory, 0 = end.
 * `name` must hold VX_NAME_MAX bytes. */
static inline int vx_readdir(const char *path, int index, char *name)
                                                    { return VX_CALL3(VX_SYS_FS_READDIR, path, index, name); }
static inline int vx_mkdir(const char *path)        { return VX_CALL1(VX_SYS_FS_MKDIR, path); }
static inline int vx_remove(const char *path)       { return VX_CALL1(VX_SYS_FS_REMOVE, path); }

/* ---- windows (available while the desktop is running) ------------------ */
/* Returns a window handle, or -1 when there is no desktop. Colours are
 * 0xRRGGBB (see VX_RGB); coordinates are relative to the client area. */
static inline int vx_win_create(const char *title, int w, int h)
                                                    { return VX_CALL3(VX_SYS_WIN_CREATE, title, w, h); }
static inline int vx_win_destroy(int win)           { return VX_CALL1(VX_SYS_WIN_DESTROY, win); }
static inline int vx_win_fill(int win, int x, int y, int w, int h, uint32_t color)
{
    struct vx_rect r = { x, y, w, h };
    return VX_CALL3(VX_SYS_WIN_FILL, win, &r, color);
}
static inline int vx_win_text(int win, int x, int y, const char *s, uint32_t color)
                                                    { return vx_syscall(VX_SYS_WIN_TEXT, win, x, y, (long)s, (long)color); }
/* Copy w*h pixels (row by row) into the window. */
static inline int vx_win_blit(int win, int x, int y, int w, int h, const uint32_t *pixels)
{
    struct vx_rect r = { x, y, w, h };
    return VX_CALL3(VX_SYS_WIN_BLIT, win, &r, pixels);
}
/* Wait for an event. timeout_ms: -1 = forever, 0 = just poll.
 * Returns 1 if *ev was filled, 0 on timeout. */
static inline int vx_win_event(int win, struct vx_event *ev, int timeout_ms)
                                                    { return VX_CALL3(VX_SYS_WIN_EVENT, win, ev, timeout_ms); }

/* ---- libvx: a tiny C library ------------------------------------------- */

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strcat(char *dst, const char *src);
char  *strrchr(const char *s, int c);
char  *strchr(const char *s, int c);
int    atoi(const char *s);

int    putchar(int c);
int    puts(const char *s);
int    getchar(void);                               /* one key, see VX_KEY_* */
char  *gets_s(char *buf, size_t max);               /* read a line with echo */
int    printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int    snprintf(char *buf, size_t max, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int    vsnprintf(char *buf, size_t max, const char *fmt, va_list ap);

void  *malloc(size_t size);                         /* all memory is released on exit */
void   free(void *ptr);
void   srand(unsigned seed);
int    rand(void);

#endif
