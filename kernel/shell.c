/* Minimal interactive kernel shell. */
#include "kernel.h"

#define LINE_MAX 128
#define ARGS_MAX 16

struct command {
    const char *name;
    const char *help;
    void (*fn)(int argc, char **argv);
};

static const struct command commands[];

static void cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (const struct command *c = commands; c->name; c++) {
        kprintf("  %s", c->name);
        for (size_t n = strlen(c->name); n < 8; n++) kputc(' ');
        kprintf("%s\n", c->help);
    }
}

static void cmd_clear(int argc, char **argv)
{
    (void)argc; (void)argv;
    vga_clear();
}

static void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) kprintf("%s%s", i > 1 ? " " : "", argv[i]);
    kputc('\n');
}

static void cmd_info(int argc, char **argv)
{
    (void)argc; (void)argv;
    u32 regs[4];
    char vendor[13];
    __asm__ volatile("cpuid" : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
                             : "a"(0));
    memcpy(vendor, &regs[1], 4);
    memcpy(vendor + 4, &regs[3], 4);
    memcpy(vendor + 8, &regs[2], 4);
    vendor[12] = 0;
    kprintf("VortexOS %s (i386, built %s)\n", VORTEX_VERSION, __DATE__);
    kprintf("CPU vendor: %s\n", vendor);
}

static void cmd_mem(int argc, char **argv)
{
    (void)argc; (void)argv;
    u32 total, used, blocks;
    heap_stats(&total, &used, &blocks);
    kprintf("RAM:  %u KiB lower, %u KiB upper\n", g_mem_lower_kb, g_mem_upper_kb);
    kprintf("Heap: %u KiB total, %u bytes used, %u block(s)\n", total / 1024, used, blocks);
}

static void cmd_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;
    u32 t = timer_ticks();
    u32 s = t / TIMER_HZ;
    kprintf("up %02u:%02u:%02u (%u ticks)\n", s / 3600, s / 60 % 60, s % 60, t);
}

static void cmd_sleep(int argc, char **argv)
{
    int ms = argc > 1 ? atoi(argv[1]) : 0;
    if (ms <= 0) { kputs("usage: sleep <milliseconds>\n"); return; }
    sleep_ms((u32)ms);
}

static void cmd_color(int argc, char **argv)
{
    int fg = argc > 1 ? atoi(argv[1]) : -1;
    if (fg < 0 || fg > 15) { kputs("usage: color <0-15>\n"); return; }
    vga_set_color((u8)fg, VGA_BLACK);
}

static void cmd_crash(int argc, char **argv)
{
    (void)argc; (void)argv;
    __asm__ volatile("ud2");            /* invalid opcode -> exception 6 */
}

static void cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    kputs("Rebooting...\n");
    cpu_cli();
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) { }
    outb(0x64, 0xFE);                   /* pulse the CPU reset line via the 8042 */
    for (;;) cpu_hlt();
}

static void cmd_halt(int argc, char **argv)
{
    (void)argc; (void)argv;
    kputs("System halted. It is now safe to power off.\n");
    cpu_cli();
    for (;;) cpu_hlt();
}

static const struct command commands[] = {
    { "help",   "list commands",                 cmd_help },
    { "clear",  "clear the screen",              cmd_clear },
    { "echo",   "print arguments",               cmd_echo },
    { "info",   "system information",            cmd_info },
    { "mem",    "memory usage",                  cmd_mem },
    { "uptime", "time since boot",               cmd_uptime },
    { "sleep",  "sleep <ms>",                    cmd_sleep },
    { "color",  "set text color <0-15>",         cmd_color },
    { "crash",  "trigger a CPU exception",       cmd_crash },
    { "reboot", "restart the machine",           cmd_reboot },
    { "halt",   "stop the CPU",                  cmd_halt },
    { NULL, NULL, NULL },
};

static void read_line(char *line)
{
    size_t n = 0;
    for (;;) {
        char c = keyboard_getchar();
        if (c == '\n') { kputc('\n'); break; }
        if (c == '\b') {
            if (n) { n--; kputc('\b'); }
        } else if (c >= ' ' && n < LINE_MAX - 1) {
            line[n++] = c;
            kputc(c);
        }
    }
    line[n] = 0;
}

static int split(char *line, char **argv)
{
    int argc = 0;
    while (*line && argc < ARGS_MAX) {
        while (*line == ' ') line++;
        if (!*line) break;
        argv[argc++] = line;
        while (*line && *line != ' ') line++;
        if (*line) *line++ = 0;
    }
    return argc;
}

void shell_run(void)
{
    char line[LINE_MAX];
    char *argv[ARGS_MAX];

    for (;;) {
        kputs("vortex> ");
        read_line(line);
        int argc = split(line, argv);
        if (!argc) continue;

        const struct command *c = commands;
        while (c->name && strcmp(c->name, argv[0]) != 0) c++;
        if (c->name) c->fn(argc, argv);
        else kprintf("%s: command not found (try 'help')\n", argv[0]);
    }
}
