#include "kernel.h"

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

struct multiboot_info {
    u32 flags;
    u32 mem_lower;                      /* KiB below 1 MiB  (flags bit 0) */
    u32 mem_upper;                      /* KiB above 1 MiB  (flags bit 0) */
};

u32 g_mem_lower_kb, g_mem_upper_kb;

static void banner(void)
{
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kputs(" __     __         _             ___  ____\n"
          " \\ \\   / /__  _ __| |_ _____  __/ _ \\/ ___|\n"
          "  \\ \\ / / _ \\| '__| __/ _ \\ \\/ / | | \\___ \\\n"
          "   \\ V / (_) | |  | ||  __/>  <| |_| |___) |\n"
          "    \\_/ \\___/|_|   \\__\\___/_/\\_\\\\___/|____/\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("\nVortexOS %s\n\n", VORTEX_VERSION);
}

static void ok(const char *what)
{
    kputs("[ ");
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    kputs("OK");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf(" ] %s\n", what);
}

void kmain(u32 magic, const struct multiboot_info *mbi)
{
    vga_init();
    serial_init();
    banner();

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
        panic("not booted by a Multiboot-compliant bootloader");
    if (mbi->flags & 1) {
        g_mem_lower_kb = mbi->mem_lower;
        g_mem_upper_kb = mbi->mem_upper;
    }

    gdt_init();                 ok("GDT");
    idt_init();                 ok("IDT + PIC");
    heap_init(g_mem_upper_kb);  ok("Kernel heap");
    timer_init();               ok("Timer (100 Hz)");
    keyboard_init();            ok("PS/2 keyboard");
    cpu_sti();

    kprintf("\nMemory: %u MiB. Type 'help' for a list of commands.\n\n",
            (g_mem_upper_kb + 1024) / 1024);
    shell_run();
}
