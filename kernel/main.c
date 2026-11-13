#include "kernel.h"

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

struct multiboot_module { u32 start, end, string, reserved; };

struct multiboot_info {
    u32 flags;
    u32 mem_lower;                      /* KiB below 1 MiB  (flags bit 0) */
    u32 mem_upper;                      /* KiB above 1 MiB  (flags bit 0) */
    u32 boot_device;
    u32 cmdline;
    u32 mods_count;                     /* boot modules     (flags bit 3) */
    u32 mods_addr;
};

extern char _kernel_end[];
u32 g_mem_lower_kb, g_mem_upper_kb;

static void banner(void)
{
    console_color(VX_LIGHT_CYAN, VX_BLACK);
    kputs(" __     __         _             ___  ____\n"
          " \\ \\   / /__  _ __| |_ _____  __/ _ \\/ ___|\n"
          "  \\ \\ / / _ \\| '__| __/ _ \\ \\/ / | | \\___ \\\n"
          "   \\ V / (_) | |  | ||  __/>  <| |_| |___) |\n"
          "    \\_/ \\___/|_|   \\__\\___/_/\\_\\\\___/|____/\n");
    console_color(VX_LIGHT_GREY, VX_BLACK);
    kprintf("\nVortexOS %s (x86-64)\n\n", VORTEX_VERSION);
}

static void status(int good, const char *what)
{
    kputs("[ ");
    console_color(good ? VX_LIGHT_GREEN : VX_YELLOW, VX_BLACK);
    kputs(good ? "OK" : "--");
    console_color(VX_LIGHT_GREY, VX_BLACK);
    kprintf(" ] %s\n", what);
}

void kmain(u64 magic, const struct multiboot_info *mbi)
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

    /* The initrd is the first boot module; the heap must start after it. */
    const u8 *initrd = NULL;
    u32 initrd_len = 0;
    u64 heap_start = (u64)_kernel_end;
    if ((mbi->flags & 8) && mbi->mods_count) {
        const struct multiboot_module *mods = (const struct multiboot_module *)(uptr)mbi->mods_addr;
        initrd = (const u8 *)(uptr)mods[0].start;
        initrd_len = mods[0].end - mods[0].start;
        for (u32 i = 0; i < mbi->mods_count; i++)
            if (mods[i].end > heap_start) heap_start = mods[i].end;
    }

    /* Memory map: [kernel + initrd][kernel heap][page frames ............]
     * The kernel uses at most the first 3 GiB of RAM (identity-mapped). */
    u64 ram_end = 0x100000 + (u64)g_mem_upper_kb * 1024;
    if (ram_end > KERNEL_LIMIT) ram_end = KERNEL_LIMIT;
    heap_start = (heap_start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (ram_end < heap_start + 24u * 1024 * 1024) panic("not enough memory (32 MiB required)");
    u64 heap_size = ((ram_end - heap_start) / 2) & ~(PAGE_SIZE - 1);
    if (heap_size > 64u * 1024 * 1024) heap_size = 64u * 1024 * 1024;

    gdt_init();                             status(1, "GDT, TSS");
    idt_init();                             status(1, "IDT, PIC, system calls");
    heap_init(heap_start, heap_size);
    pmm_init(heap_start + heap_size, ram_end);
    vmm_init();
    gfx_probe();
    status(1, "Long mode, 4-level paging, heap, page frames");
    task_init();                            status(1, "Scheduler, user mode");
    timer_init();
    keyboard_init();
    mouse_init();                           status(1, "Timer, keyboard, mouse");

    fs_init();
    int files = initrd ? fs_load_tar(initrd, initrd_len) : 0;
    kprintf("[ %s ] Root filesystem: %d file(s) from initrd\n", files ? "OK" : "--", files);
    u32 disk_kb = fat_mount();
    struct fat_info fi;
    fat_info(&fi);
    if (disk_kb) kprintf("[ OK ] Disk: FAT%d volume, %u KiB, mounted at /disk\n", fi.type, disk_kb);
    else status(0, "No FAT disk (changes are kept in RAM only)");
    status(gfx_available(), gfx_available() ? "Graphics adapter (Bochs VBE)"
                                            : "No graphics adapter: text mode only");

    env_set("PATH", "/bin:/apps");
    env_set("HOME", "/home");
    cpu_sti();

    kprintf("\nMemory: %u MiB.\n", (g_mem_upper_kb + 1024) / 1024);
    if (shell_run_script("/etc/init.sh") < 0)
        kputs("Type 'help' for a list of commands.\n\n");
    shell_run();
}
