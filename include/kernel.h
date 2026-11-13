#ifndef VORTEX_KERNEL_H
#define VORTEX_KERNEL_H

#define VORTEX_VERSION "0.1.0"

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
static inline void io_wait(void) { outb(0x80, 0); }
static inline void cpu_sti(void) { __asm__ volatile("sti"); }
static inline void cpu_cli(void) { __asm__ volatile("cli"); }
static inline void cpu_hlt(void) { __asm__ volatile("hlt"); }

/* ---- string.c ---------------------------------------------------------- */
void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    atoi(const char *s);

/* ---- vga.c ------------------------------------------------------------- */
enum { VGA_BLACK, VGA_BLUE, VGA_GREEN, VGA_CYAN, VGA_RED, VGA_MAGENTA,
       VGA_BROWN, VGA_LIGHT_GREY, VGA_DARK_GREY, VGA_LIGHT_BLUE,
       VGA_LIGHT_GREEN, VGA_LIGHT_CYAN, VGA_LIGHT_RED, VGA_LIGHT_MAGENTA,
       VGA_YELLOW, VGA_WHITE };
void vga_init(void);
void vga_clear(void);
void vga_set_color(u8 fg, u8 bg);
void vga_putc(char c);

/* ---- serial.c (COM1, mirrors console output) --------------------------- */
void serial_init(void);
void serial_putc(char c);

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
};
typedef void (*irq_handler_t)(struct regs *);
void gdt_init(void);
void idt_init(void);
void irq_install(int irq, irq_handler_t handler);

/* ---- timer.c ----------------------------------------------------------- */
#define TIMER_HZ 100
void timer_init(void);
u32  timer_ticks(void);
void sleep_ms(u32 ms);

/* ---- keyboard.c -------------------------------------------------------- */
void keyboard_init(void);
char keyboard_getchar(void);            /* blocks until a key is pressed */

/* ---- heap.c ------------------------------------------------------------ */
void  heap_init(u32 mem_upper_kb);
void *kmalloc(size_t size);
void  kfree(void *ptr);
void  heap_stats(u32 *total, u32 *used, u32 *blocks);

/* ---- shell.c ----------------------------------------------------------- */
void shell_run(void) __attribute__((noreturn));

extern u32 g_mem_lower_kb, g_mem_upper_kb;

#endif
