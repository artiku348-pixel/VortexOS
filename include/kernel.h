#ifndef VORTEX_KERNEL_H
#define VORTEX_KERNEL_H

#include <vortex/abi.h>

#define VORTEX_VERSION "0.3.0"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed int     i32;
typedef u32            size_t;
#define NULL ((void *)0)

/* ---- port I/O ---------------------------------------------------------- */
static inline void outb(u16 port, u8 val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline u8 inb(u16 port)
{
    u8 r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
static inline void outw(u16 port, u16 val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline u16 inw(u16 port)
{
    u16 r;
    __asm__ volatile("inw %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
static inline void outl(u16 port, u32 val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline u32 inl(u16 port)
{
    u32 r;
    __asm__ volatile("inl %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
static inline void io_wait(void) { outb(0x80, 0); }
static inline void cpu_sti(void) { __asm__ volatile("sti"); }
static inline void cpu_cli(void) { __asm__ volatile("cli"); }
static inline void cpu_hlt(void) { __asm__ volatile("hlt"); }

/* ---- string.c ---------------------------------------------------------- */
void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void   memset32(u32 *dst, u32 value, size_t count);
void   memcpy32(u32 *dst, const u32 *src, size_t count);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    atoi(const char *s);
char  *strcpy(char *dst, const char *src);
void   strlcpy(char *dst, const char *src, size_t max);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);

/* ---- vga.c (80x25 text mode) ------------------------------------------- */
void vga_init(void);
void vga_clear(void);
void vga_set_color(u8 fg, u8 bg);
void vga_putc(char c);

/* ---- serial.c (COM1, mirrors console output) --------------------------- */
void serial_init(void);
void serial_putc(char c);

/* ---- gfx.c (linear framebuffer + drawing) ------------------------------ */
struct surface {
    u32 *pix;                           /* 0x00RRGGBB */
    int w, h;
};
extern const u8 font8x16[95][16];
#define GFX_FB_SIZE (4u * 1024 * 1024)   /* framebuffer bytes mapped by the kernel */
int  gfx_available(void);               /* Bochs/QEMU VBE adapter present */
u32  gfx_probe(void);                   /* framebuffer address (0 = none); call before paging */
int  gfx_active(void);                  /* graphics mode is on */
int  gfx_init(void);                    /* switch to 1024x768x32; 0 = ok */
struct surface *gfx_screen(void);
void gfx_fill(struct surface *s, int x, int y, int w, int h, u32 color);
void gfx_frame(struct surface *s, int x, int y, int w, int h, u32 color);
void gfx_char(struct surface *s, int x, int y, char c, u32 fg);
void gfx_char_bg(struct surface *s, int x, int y, char c, u32 fg, u32 bg);
void gfx_text(struct surface *s, int x, int y, const char *str, u32 fg);
void gfx_blit(struct surface *dst, int dx, int dy, const u32 *src, int sw, int sh);

/* ---- term.c (text terminal drawn on a surface) ------------------------- */
struct term {
    struct surface *surf;
    int cols, rows, col, row;
    u32 fg, bg;
};
void term_init(struct term *t, struct surface *surf);
void term_putc(struct term *t, char c);
void term_clear(struct term *t);
void term_color(struct term *t, u8 fg, u8 bg);

/* ---- console.c --------------------------------------------------------- */
struct console {
    struct term *term;                  /* NULL = kernel console */
    volatile u8 in[64];
    volatile u32 head, tail;
    int fg_task;                        /* foreground program: Ctrl+C kills it */
    int interrupted;
};
struct capture { char *buf; u32 len, cap; };    /* output redirection */
extern struct console kcon;
void console_color(u8 fg, u8 bg);
void console_clear(void);
int  console_getchar(void);             /* blocks; returns a VX_KEY_* code */
int  console_peek(struct console *c);   /* next key or -1 */
void console_push_key(struct console *c, int key);
void console_takeover(void);            /* full-screen terminal on the framebuffer */
void console_emergency(void);           /* make kernel output visible for a panic */

/* ---- printf.c ---------------------------------------------------------- */
void kputc(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void panic(const char *msg) __attribute__((noreturn));

/* ---- gdt.c / idt.c ----------------------------------------------------- */
struct regs {
    u32 es, ds;
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax;
    u32 int_no, err_code;
    u32 eip, cs, eflags;
    u32 user_esp, user_ss;              /* only pushed when coming from ring 3 */
};
typedef void (*irq_handler_t)(struct regs *);
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE 0x1B                  /* ring 3 selectors (RPL = 3) */
#define SEL_UDATA 0x23
void gdt_init(void);
void tss_set_kernel_stack(u32 esp0);    /* stack used when ring 3 enters the kernel */
void enter_user(u32 entry, u32 user_esp) __attribute__((noreturn));
void idt_init(void);
void irq_install(int irq, irq_handler_t handler);

/* ---- timer.c / rtc.c --------------------------------------------------- */
#define TIMER_HZ VX_TICKS_PER_SEC
void timer_init(void);
u32  timer_ticks(void);
void sleep_ms(u32 ms);
void rtc_read(struct vx_time *t);

/* ---- keyboard.c / mouse.c ---------------------------------------------- */
void keyboard_init(void);
int  keyboard_poll(void);               /* key code or -1 */
struct mouse_packet { int dx, dy, buttons; };
void mouse_init(void);
int  mouse_poll(struct mouse_packet *p);

/* ---- pmm.c / vmm.c (physical frames, paging) --------------------------- */
#define PAGE_SIZE    4096u
#define KERNEL_LIMIT 0x40000000u        /* kernel space: identity-mapped RAM below 1 GiB */
#define USER_BASE    0x40000000u        /* user space: [USER_BASE, USER_TOP) */
#define USER_TOP     0x80000000u
void pmm_init(u32 start, u32 end);
u32  pmm_alloc(void);                   /* zeroed frame (physical address) or 0 */
void pmm_free(u32 addr);
void pmm_stats(u32 *total, u32 *free);
void vmm_init(u32 ram_end, u32 framebuffer, u32 framebuffer_size);
u32 *vmm_kernel_dir(void);
u32 *vmm_new_dir(void);
void vmm_free_dir(u32 *dir);
void vmm_switch(u32 *dir);              /* NULL = kernel directory */
int  vmm_alloc(u32 *dir, u32 virt, u32 size);
int  vmm_copy_to(u32 *dir, u32 virt, const void *src, u32 len);
int  user_ok(const void *ptr, u32 len);
int  user_str(const char *s, u32 max);

/* ---- heap.c ------------------------------------------------------------ */
void  heap_init(u32 start, u32 size);
void *kmalloc(size_t size);
void  kfree(void *ptr);
void  heap_stats(u32 *total, u32 *used, u32 *blocks);

/* ---- task.c (cooperative kernel threads) ------------------------------- */
#define TASK_MAX 32
enum { TASK_FREE, TASK_READY, TASK_ZOMBIE };
struct task {
    int   id, state, exit_code;
    u32   esp;
    void *stack;
    char  name[24];
    void (*entry)(void *);
    void *arg;
    struct console *con;                /* where kprintf / getchar go */
    struct console *own_con;            /* console destroyed with this task */
    struct capture *cap;
    void *allocs;                       /* memory released on exit */
    u32  *pagedir;                      /* user process: its address space (else NULL) */
    u32   brk;                          /* user process: end of the heap */
    char  cwd[VX_PATH_MAX];
};
void  task_init(void);
struct task *task_current(void);
struct task *task_slot(int index);      /* for `ps`; may be TASK_FREE */
struct task *task_create(const char *name, void (*entry)(void *), void *arg);
void  task_yield(void);
void  task_poke(void);
void  task_exit(int code) __attribute__((noreturn));
int   task_kill(int id);
int   task_alive(int id);
int   task_wait(int id);                /* returns 1 if interrupted by Ctrl+C */
void *task_alloc(struct task *t, size_t size);
void  task_free(struct task *t, void *ptr);

/* ---- fs.c (in-memory filesystem, filled from the initrd) --------------- */
struct fs_node {
    char name[VX_NAME_MAX];
    int  is_dir;
    u8  *data;
    u32  size, cap;
    struct fs_node *parent, *children, *next;
};
void fs_init(void);
int  fs_load_tar(const u8 *tar, u32 len);       /* returns number of files */
struct fs_node *fs_lookup(const char *path);    /* relative to the task's cwd */
struct fs_node *fs_create(const char *path, int is_dir);
int  fs_write(struct fs_node *n, const void *data, u32 len, int append);
int  fs_remove(const char *path);
void fs_abspath(const struct fs_node *n, char *buf, size_t max);

/* ---- env.c ------------------------------------------------------------- */
const char *env_get(const char *name);
int  env_set(const char *name, const char *value);
void env_unset(const char *name);
int  env_at(int index, const char **name, const char **value);

/* ---- exec.c / syscall.c ------------------------------------------------ */
struct fs_node *exec_find(const char *name);    /* searches $PATH */
int  exec_is_elf(const struct fs_node *n);
int  exec_spawn(struct fs_node *n, int argc, char **argv);  /* task id or -1 */
void syscall_dispatch(struct regs *r);

/* ---- desktop.c (window manager) ---------------------------------------- */
struct window;
int  wm_running(void);
void wm_mark_dirty(void);
void wm_destroy_owned(int task_id);
int  wm_create(int owner, const char *title, int w, int h);
int  wm_destroy(int owner, int id);
int  wm_fill(int owner, int id, const struct vx_rect *r, u32 color);
int  wm_text(int owner, int id, int x, int y, const char *s, u32 color);
int  wm_blit(int owner, int id, const struct vx_rect *r, const u32 *pixels);
int  wm_event(int owner, int id, struct vx_event *ev);
void desktop_run(void);

/* ---- shell.c ----------------------------------------------------------- */
void shell_run(void) __attribute__((noreturn));
int  shell_run_script(const char *path);

extern u32 g_mem_lower_kb, g_mem_upper_kb;

#endif
