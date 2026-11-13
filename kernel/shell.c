/* Command shell: built-in commands, programs and scripts found via $PATH,
 * variables ($NAME, $1..$9), output redirection (> and >>) and background
 * jobs (&). Each terminal runs its own instance in its own task. */
#include "kernel.h"

#define LINE_MAX  256
#define ARGS_MAX  16
#define DEPTH_MAX 8

struct sh_args { int argc; char **argv; };      /* script arguments */

struct command {
    const char *name;
    const char *help;
    int (*fn)(int argc, char **argv);
};

static const struct command commands[];
static int run_line(const char *raw, const struct sh_args *args, int depth);

static void pad(size_t printed, size_t width)
{
    while (printed++ < width) kputc(' ');
}

/* ---- built-in commands ------------------------------------------------- */

static int cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    int col = 0;
    for (const struct command *c = commands; c->name; c++) {
        kprintf("%s", c->name);
        pad(strlen(c->name), 9);
        kprintf("%s", c->help);
        if (col++ & 1) kputc('\n');
        else pad(strlen(c->help), 30);
    }
    if (col & 1) kputc('\n');
    kputs("Anything else is looked up in $PATH (programs and scripts).\n");
    return 0;
}

static int cmd_clear(int argc, char **argv)
{
    (void)argc; (void)argv;
    console_clear();
    return 0;
}

static int cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) kprintf("%s%s", i > 1 ? " " : "", argv[i]);
    kputc('\n');
    return 0;
}

static int cmd_info(int argc, char **argv)
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
    kprintf("VortexOS %s (x86-64, built %s)\n", VORTEX_VERSION, __DATE__);
    kprintf("CPU vendor: %s\n", vendor);
    kprintf("Graphics:   %s\n", gfx_active() ? "1024x768x32 framebuffer"
                              : gfx_available() ? "available (run 'desktop')" : "text mode only");
    return 0;
}

static int cmd_mem(int argc, char **argv)
{
    (void)argc; (void)argv;
    u32 total, used, blocks;
    heap_stats(&total, &used, &blocks);
    kprintf("RAM:  %u KiB lower, %u KiB upper\n", g_mem_lower_kb, g_mem_upper_kb);
    kprintf("Heap: %u KiB total, %u KiB used, %u block(s)\n", total, used, blocks);
    u32 frames, free;
    pmm_stats(&frames, &free);
    kprintf("Page frames: %u total, %u free (%u KiB free for programs)\n", frames, free, free * 4);
    return 0;
}

static int cmd_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;
    u32 t = timer_ticks();
    u32 s = t / TIMER_HZ;
    kprintf("up %02u:%02u:%02u (%u ticks)\n", s / 3600, s / 60 % 60, s % 60, t);
    return 0;
}

static int cmd_date(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct vx_time t;
    rtc_read(&t);
    kprintf("%04d-%02d-%02d %02d:%02d:%02d\n", t.year, t.month, t.day, t.hour, t.minute, t.second);
    return 0;
}

static int cmd_sleep(int argc, char **argv)
{
    int ms = argc > 1 ? atoi(argv[1]) : 0;
    if (ms <= 0) { kputs("usage: sleep <milliseconds>\n"); return 1; }
    sleep_ms((u32)ms);
    return 0;
}

static int cmd_color(int argc, char **argv)
{
    int fg = argc > 1 ? atoi(argv[1]) : -1;
    if (fg < 0 || fg > 15) { kputs("usage: color <0-15>\n"); return 1; }
    console_color((u8)fg, VX_BLACK);
    return 0;
}

static int cmd_pwd(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("%s\n", task_current()->cwd);
    return 0;
}

static int cmd_cd(int argc, char **argv)
{
    const char *dest = argc > 1 ? argv[1] : env_get("HOME");
    struct fs_node *n = fs_lookup(dest ? dest : "/");
    if (!n || !n->is_dir) { kprintf("cd: %s: no such directory\n", dest); return 1; }
    fs_abspath(n, task_current()->cwd, VX_PATH_MAX);
    return 0;
}

static int cmd_ls(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : ".";
    struct fs_node *n = fs_lookup(path);
    if (!n) { kprintf("ls: %s: not found\n", path); return 1; }
    if (!n->is_dir) { kprintf("%8u  %s\n", n->size, n->name); return 0; }
    if (fs_open(n)) { kprintf("ls: %s: read error\n", path); return 1; }
    for (n = n->children; n; n = n->next) {
        if (n->is_dir) kprintf("   <dir>  %s/\n", n->name);
        else kprintf("%8u  %s\n", n->size, n->name);
    }
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) { kputs("usage: cat <file>...\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        struct fs_node *n = fs_lookup(argv[i]);
        if (!n || n->is_dir) { kprintf("cat: %s: no such file\n", argv[i]); return 1; }
        if (fs_open(n)) { kprintf("cat: %s: read error\n", argv[i]); return 1; }
        for (u32 k = 0; k < n->size; k++) kputc((char)n->data[k]);
    }
    return 0;
}

static int cmd_mkdir(int argc, char **argv)
{
    if (argc < 2) { kputs("usage: mkdir <dir>\n"); return 1; }
    if (!fs_create(argv[1], 1)) { kprintf("mkdir: cannot create %s\n", argv[1]); return 1; }
    return 0;
}

static int cmd_touch(int argc, char **argv)
{
    if (argc < 2) { kputs("usage: touch <file>\n"); return 1; }
    if (!fs_create(argv[1], 0)) { kprintf("touch: cannot create %s\n", argv[1]); return 1; }
    return 0;
}

static int cmd_rm(int argc, char **argv)
{
    if (argc < 2) { kputs("usage: rm <file|empty dir>\n"); return 1; }
    int r = fs_remove(argv[1]);
    if (r == -2) kprintf("rm: %s: directory not empty\n", argv[1]);
    else if (r) kprintf("rm: %s: not found\n", argv[1]);
    return r ? 1 : 0;
}

static int copy_file(const char *from, const char *to)
{
    char path[VX_PATH_MAX];
    struct fs_node *src = fs_lookup(from), *dst = fs_lookup(to);
    if (!src || src->is_dir || fs_open(src)) { kprintf("%s: no such file\n", from); return 1; }
    if (dst == src) { kputs("source and destination are the same file\n"); return 1; }
    if (dst && dst->is_dir) {           /* copy into a directory */
        fs_abspath(dst, path, sizeof(path));
        if (strlen(path) + strlen(src->name) + 2 > sizeof(path)) return 1;
        if (path[1]) strcpy(path + strlen(path), "/");
        strcpy(path + strlen(path), src->name);
        to = path;
    }
    dst = fs_create(to, 0);
    if (!dst || dst == src || fs_write(dst, src->data, src->size, 0) < 0) {
        kprintf("cannot write %s\n", to);
        return 1;
    }
    return 0;
}

static int cmd_cp(int argc, char **argv)
{
    if (argc < 3) { kputs("usage: cp <from> <to>\n"); return 1; }
    return copy_file(argv[1], argv[2]);
}

static int cmd_mv(int argc, char **argv)
{
    if (argc < 3) { kputs("usage: mv <from> <to>\n"); return 1; }
    if (copy_file(argv[1], argv[2])) return 1;
    fs_remove(argv[1]);
    return 0;
}

static int cmd_env(int argc, char **argv)
{
    (void)argc; (void)argv;
    const char *name, *value;
    for (int i = 0; env_at(i, &name, &value); i++)
        if (name) kprintf("%s=%s\n", name, value);
    return 0;
}

/* set NAME=VALUE | set NAME VALUE... */
static int cmd_set(int argc, char **argv)
{
    char value[VX_PATH_MAX] = "";
    if (argc < 2) return cmd_env(argc, argv);

    char *eq = strchr(argv[1], '=');
    if (eq) {
        *eq = 0;
        strlcpy(value, eq + 1, sizeof(value));
    }
    for (int i = 2; i < argc; i++) {
        size_t len = strlen(value);
        if (len + strlen(argv[i]) + 2 > sizeof(value)) break;
        if (len) value[len++] = ' ';
        strcpy(value + len, argv[i]);
    }
    if (env_set(argv[1], value)) { kputs("set: bad name or value too long\n"); return 1; }
    return 0;
}

static int cmd_unset(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) env_unset(argv[i]);
    return 0;
}

static int cmd_which(int argc, char **argv)
{
    char path[VX_PATH_MAX];
    if (argc < 2) { kputs("usage: which <command>\n"); return 1; }
    for (const struct command *c = commands; c->name; c++)
        if (strcmp(c->name, argv[1]) == 0) { kprintf("%s: shell built-in\n", argv[1]); return 0; }
    struct fs_node *n = exec_find(argv[1]);
    if (!n) { kprintf("%s: not found\n", argv[1]); return 1; }
    fs_abspath(n, path, sizeof(path));
    kprintf("%s (%s)\n", path, exec_is_elf(n) ? "program" : "script");
    return 0;
}

static int cmd_ps(int argc, char **argv)
{
    (void)argc; (void)argv;
    kputs("  ID  MODE    NAME\n");
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = task_slot(i);
        if (t->state == TASK_READY)
            kprintf("%4d  %s  %s\n", t->id, t->pagedir ? "user  " : "kernel", t->name);
    }
    return 0;
}

static int cmd_kill(int argc, char **argv)
{
    if (argc < 2) { kputs("usage: kill <task id>\n"); return 1; }
    if (task_kill(atoi(argv[1]))) { kprintf("kill: cannot kill task %s\n", argv[1]); return 1; }
    return 0;
}

static int cmd_desktop(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (wm_running()) { kputs("The desktop is already running.\n"); return 1; }
    if (task_current()->id != 0) { kputs("desktop: must be started from the main console\n"); return 1; }
    desktop_run();
    return 0;
}

static int cmd_exit(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (task_current()->id == 0) { kputs("This is the main shell; use 'halt' or 'reboot'.\n"); return 1; }
    task_exit(0);
}

static int cmd_crash(int argc, char **argv)
{
    (void)argc; (void)argv;
    __asm__ volatile("ud2");            /* invalid opcode -> exception 6 */
    return 0;
}

static int cmd_sync(int argc, char **argv)
{
    (void)argc; (void)argv;
    int r = fat_sync();
    if (r == -1) kputs("sync: no disk, or write error\n");
    if (r == -2) kputs("sync: no space left on /disk\n");
    return r;
}

static int cmd_disk(int argc, char **argv)
{
    struct fat_info i;
    (void)argc; (void)argv;
    fat_info(&i);
    if (!i.type) { kputs("No FAT disk. Files live in RAM and are lost on reboot.\n"); return 1; }
    kprintf("ATA disk: %u KiB", ata_sectors() / 2);
    if (i.partition_lba) kprintf(", partition at sector %u", i.partition_lba);
    kprintf("\nVolume: FAT%d, %u-byte clusters, mounted at /disk\n", i.type, i.cluster_bytes);
    kprintf("Space: %u KiB total, %u KiB free\n", i.total_kb, i.free_kb);
    kprintf("Writes: %u sync(s), %s\n", i.syncs, i.pending == 2 ? "DISK FULL, some changes not saved"
            : i.pending ? "changes waiting to be written" : "everything is saved");
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    fat_sync();
    kputs("Rebooting...\n");
    cpu_cli();
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) { }
    outb(0x64, 0xFE);                   /* pulse the CPU reset line via the 8042 */
    for (;;) cpu_hlt();
    return 0;
}

static int cmd_halt(int argc, char **argv)
{
    (void)argc; (void)argv;
    fat_sync();
    kputs("System halted. It is now safe to power off.\n");
    cpu_cli();
    for (;;) cpu_hlt();
    return 0;
}

static const struct command commands[] = {
    { "help",    "list commands",              cmd_help },
    { "clear",   "clear the screen",           cmd_clear },
    { "echo",    "print arguments",            cmd_echo },
    { "ls",      "list a directory",           cmd_ls },
    { "cd",      "change directory",           cmd_cd },
    { "pwd",     "print current directory",    cmd_pwd },
    { "cat",     "print files",                cmd_cat },
    { "cp",      "copy a file",                cmd_cp },
    { "mv",      "move / rename a file",       cmd_mv },
    { "rm",      "remove file or empty dir",   cmd_rm },
    { "mkdir",   "create a directory",         cmd_mkdir },
    { "touch",   "create an empty file",       cmd_touch },
    { "set",     "set NAME=VALUE",             cmd_set },
    { "unset",   "remove a variable",          cmd_unset },
    { "env",     "list variables",             cmd_env },
    { "which",   "locate a command",           cmd_which },
    { "sh",      "run a script: sh FILE ARGS", NULL },
    { "ps",      "list tasks",                 cmd_ps },
    { "kill",    "kill a task by id",          cmd_kill },
    { "desktop", "start the graphical desktop", cmd_desktop },
    { "info",    "system information",         cmd_info },
    { "mem",     "memory usage",               cmd_mem },
    { "date",    "current date and time",      cmd_date },
    { "uptime",  "time since boot",            cmd_uptime },
    { "sleep",   "sleep <ms>",                 cmd_sleep },
    { "color",   "text color <0-15>",          cmd_color },
    { "crash",   "trigger a CPU exception",    cmd_crash },
    { "exit",    "close this shell",           cmd_exit },
    { "disk",    "disk and /disk usage",       cmd_disk },
    { "sync",    "write /disk changes now",    cmd_sync },
    { "reboot",  "restart the machine",        cmd_reboot },
    { "halt",    "stop the CPU",               cmd_halt },
    { NULL, NULL, NULL },
};

/* ---- parsing and execution --------------------------------------------- */

static int is_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* Substitute $NAME (environment) and $0..$9 (script arguments). */
static void expand(const char *src, char *dst, size_t max, const struct sh_args *args)
{
    size_t n = 0;
    while (*src && n + 1 < max) {
        if (*src != '$' || !is_name_char(src[1])) { dst[n++] = *src++; continue; }
        const char *value = NULL;
        char name[VX_NAME_MAX];
        size_t len = 0;
        src++;
        if (*src >= '0' && *src <= '9') {
            int idx = *src++ - '0';
            if (args && idx < args->argc) value = args->argv[idx];
        } else {
            while (is_name_char(*src) && len + 1 < sizeof(name)) name[len++] = *src++;
            name[len] = 0;
            value = env_get(name);
        }
        while (value && *value && n + 1 < max) dst[n++] = *value++;
    }
    dst[n] = 0;
}

/* Split on spaces; "double quotes" keep spaces inside one argument. */
static int split(char *line, char **argv)
{
    int argc = 0;
    while (*line && argc < ARGS_MAX) {
        while (*line == ' ' || *line == '\t') line++;
        if (!*line) break;
        if (*line == '"') {
            argv[argc++] = ++line;
            while (*line && *line != '"') line++;
        } else {
            argv[argc++] = line;
            while (*line && *line != ' ' && *line != '\t') line++;
        }
        if (*line) *line++ = 0;
    }
    return argc;
}

static int run_script(struct fs_node *n, int argc, char **argv, int depth)
{
    if (depth > DEPTH_MAX) { kputs("sh: scripts nested too deeply\n"); return 1; }

    struct task *t = task_current();
    struct sh_args args = { argc, argv };
    if (fs_open(n)) { kputs("sh: read error\n"); return 1; }
    char *text = task_alloc(t, n->size + 1);        /* the file may change while we run */
    if (!text) { kputs("sh: out of memory\n"); return 1; }
    memcpy(text, n->data, n->size);
    text[n->size] = 0;

    int status = 0;
    for (char *line = text; *line; ) {
        char *end = line;
        while (*end && *end != '\n') end++;
        char *next = *end ? end + 1 : end;
        if (end > line && end[-1] == '\r') end--;
        *end = 0;
        status = run_line(line, &args, depth);
        line = next;
    }
    task_free(t, text);
    return status;
}

static int dispatch(int argc, char **argv, int depth, int background)
{
    if (strcmp(argv[0], "sh") == 0) {
        struct fs_node *n = argc > 1 ? fs_lookup(argv[1]) : NULL;
        if (!n || n->is_dir) { kputs("usage: sh <script> [args]\n"); return 1; }
        return run_script(n, argc - 1, argv + 1, depth + 1);
    }
    for (const struct command *c = commands; c->name; c++)
        if (strcmp(c->name, argv[0]) == 0) return c->fn(argc, argv);

    struct fs_node *n = exec_find(argv[0]);
    if (!n) { kprintf("%s: command not found (try 'help')\n", argv[0]); return 127; }
    if (!exec_is_elf(n)) return run_script(n, argc, argv, depth + 1);

    int id = exec_spawn(n, argc, argv);
    if (id < 0) { kprintf("%s: cannot execute\n", argv[0]); return 126; }
    if (background) kprintf("[%d] %s\n", id, argv[0]);
    else if (task_wait(id)) kputs("^C\n");
    return 0;
}

static int run_line(const char *raw, const struct sh_args *args, int depth)
{
    char line[LINE_MAX];
    char *argv[ARGS_MAX + 1];

    while (*raw == ' ' || *raw == '\t') raw++;
    if (!*raw || *raw == '#') return 0;             /* blank line or comment */
    expand(raw, line, sizeof(line), args);
    int argc = split(line, argv);
    if (!argc) return 0;

    int background = 0;
    if (argc > 1 && strcmp(argv[argc - 1], "&") == 0) { background = 1; argc--; }

    const char *out = NULL;                         /* > file, >> file */
    int append = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], ">") == 0 || strcmp(argv[i], ">>") == 0) {
            if (i + 1 >= argc || background) { kputs("sh: bad redirection\n"); return 1; }
            append = argv[i][1] == '>';
            out = argv[i + 1];
            argc = i;
            break;
        }
    argv[argc] = NULL;
    if (!out) return dispatch(argc, argv, depth, background);

    struct task *t = task_current();
    struct capture cap = { NULL, 0, 0 }, *saved = t->cap;
    t->cap = &cap;
    int status = dispatch(argc, argv, depth, 0);
    t->cap = saved;

    struct fs_node *n = fs_create(out, 0);
    if (!n || fs_write(n, cap.buf ? cap.buf : "", cap.len, append) < 0) {
        kprintf("sh: cannot write %s\n", out);
        status = 1;
    }
    kfree(cap.buf);
    return status;
}

int shell_run_script(const char *path)
{
    char *argv[2] = { (char *)path, NULL };
    struct fs_node *n = fs_lookup(path);
    if (!n || n->is_dir) return -1;
    return run_script(n, 1, argv, 1);
}

static void read_line(char *line)
{
    size_t n = 0;
    for (;;) {
        int c = console_getchar();
        if (c == '\n') { kputc('\n'); break; }
        if (c == 3) { kputs("^C\n"); n = 0; break; }       /* Ctrl+C: drop the line */
        if (c == '\b') {
            if (n) { n--; kputc('\b'); }
        } else if (c >= ' ' && c < 127 && n < LINE_MAX - 1) {
            line[n++] = (char)c;
            kputc((char)c);
        }
    }
    line[n] = 0;
}

void shell_run(void)
{
    char line[LINE_MAX];
    for (;;) {
        console_color(VX_LIGHT_GREEN, VX_BLACK);
        kprintf("vortex:%s", task_current()->cwd);
        console_color(VX_LIGHT_GREY, VX_BLACK);
        kputs("> ");
        read_line(line);
        run_line(line, NULL, 0);
    }
}
