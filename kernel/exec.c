/* Program loader: runs position-independent ELF32 executables (built with
 * the SDK) as tasks. The image is copied to heap memory and relocated. */
#include "kernel.h"

struct elf_hdr {
    u8  ident[16];
    u16 type, machine;
    u32 version, entry, phoff, shoff, flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct elf_phdr { u32 type, offset, vaddr, paddr, filesz, memsz, flags, align; };
struct elf_dyn  { i32 tag; u32 val; };
struct elf_rel  { u32 offset, info; };

enum { PT_LOAD = 1, PT_DYNAMIC = 2, ET_DYN = 3, EM_386 = 3 };
enum { DT_NULL = 0, DT_REL = 17, DT_RELSZ = 18, R_386_NONE = 0, R_386_RELATIVE = 8 };

struct app_start {
    int (*entry)(int, char **);
    int argc;
    char **argv;
};

int exec_is_elf(const struct fs_node *n)
{
    return n && !n->is_dir && n->size >= sizeof(struct elf_hdr) &&
           memcmp(n->data, "\x7f" "ELF", 4) == 0;
}

struct fs_node *exec_find(const char *name)
{
    struct fs_node *n;
    if (strchr(name, '/')) {
        n = fs_lookup(name);
        return n && !n->is_dir ? n : NULL;
    }
    const char *path = env_get("PATH");
    if (!path) path = "/bin";
    while (*path) {
        char full[VX_PATH_MAX];
        size_t len = 0;
        while (path[len] && path[len] != ':') len++;
        if (len && len + 1 + strlen(name) < sizeof(full)) {
            memcpy(full, path, len);
            full[len] = '/';
            strcpy(full + len + 1, name);
            n = fs_lookup(full);
            if (n && !n->is_dir) return n;
        }
        path += len;
        if (*path == ':') path++;
    }
    return NULL;
}

static void app_main(void *arg)
{
    struct app_start *s = arg;
    task_exit(s->entry(s->argc, s->argv));
}

/* Returns the load base, or NULL. `*entry` receives the relocated entry. */
static u8 *load_image(struct task *t, const struct fs_node *n, u32 *entry)
{
    const struct elf_hdr *eh = (const struct elf_hdr *)n->data;
    if (eh->ident[4] != 1 || eh->type != ET_DYN || eh->machine != EM_386) return NULL;
    if (eh->phentsize != sizeof(struct elf_phdr) || eh->phnum > 32 ||
        eh->phoff > n->size || eh->phoff + eh->phnum * sizeof(struct elf_phdr) > n->size)
        return NULL;

    const struct elf_phdr *ph = (const struct elf_phdr *)(n->data + eh->phoff);
    const struct elf_phdr *dynamic = NULL;
    u32 lo = 0xFFFFFFFF, hi = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == PT_DYNAMIC) dynamic = &ph[i];
        if (ph[i].type != PT_LOAD) continue;
        if (ph[i].filesz > ph[i].memsz || ph[i].offset > n->size ||
            ph[i].filesz > n->size - ph[i].offset || ph[i].memsz > 0x4000000 ||
            ph[i].vaddr > 0x4000000)
            return NULL;
        if (ph[i].vaddr < lo) lo = ph[i].vaddr;
        if (ph[i].vaddr + ph[i].memsz > hi) hi = ph[i].vaddr + ph[i].memsz;
    }
    if (hi <= lo || eh->entry < lo || eh->entry >= hi) return NULL;

    u32 span = hi - lo;
    u8 *raw = task_alloc(t, span + 16);
    if (!raw) return NULL;
    u8 *base = (u8 *)(((u32)raw + 15) & ~15u);
    u32 delta = (u32)base - lo;         /* run address = link address + delta */
    memset(base, 0, span);
    for (int i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD)
            memcpy(base + (ph[i].vaddr - lo), n->data + ph[i].offset, ph[i].filesz);

    if (dynamic) {
        u32 rel = 0, relsz = 0;
        if (dynamic->vaddr < lo || dynamic->vaddr + dynamic->memsz > hi) return NULL;
        const struct elf_dyn *d = (const struct elf_dyn *)(dynamic->vaddr + delta);
        for (u32 i = 0; i < dynamic->memsz / sizeof(*d) && d[i].tag != DT_NULL; i++) {
            if (d[i].tag == DT_REL) rel = d[i].val;
            if (d[i].tag == DT_RELSZ) relsz = d[i].val;
        }
        if (relsz && (rel < lo || rel + relsz > hi)) return NULL;
        const struct elf_rel *r = (const struct elf_rel *)(rel + delta);
        for (u32 i = 0; i < relsz / sizeof(*r); i++) {
            u32 type = r[i].info & 0xFF;
            if (type == R_386_NONE) continue;
            if (type != R_386_RELATIVE || r[i].offset < lo || r[i].offset + 4 > hi) return NULL;
            *(u32 *)(r[i].offset + delta) += delta;
        }
    }
    *entry = eh->entry + delta;
    return base;
}

int exec_spawn(struct fs_node *n, int argc, char **argv)
{
    if (!exec_is_elf(n)) return -1;

    struct task *t = task_create(n->name, app_main, NULL);
    if (!t) return -1;

    /* Copy argv into memory owned by the new task. */
    size_t bytes = sizeof(struct app_start) + (size_t)(argc + 1) * sizeof(char *);
    for (int i = 0; i < argc; i++) bytes += strlen(argv[i]) + 1;
    struct app_start *s = task_alloc(t, bytes);
    u32 entry = 0;
    if (!s || !load_image(t, n, &entry)) {
        task_kill(t->id);
        return -1;
    }
    s->entry = (int (*)(int, char **))entry;
    s->argc = argc;
    s->argv = (char **)(s + 1);
    char *str = (char *)(s->argv + argc + 1);
    for (int i = 0; i < argc; i++) {
        s->argv[i] = str;
        strcpy(str, argv[i]);
        str += strlen(argv[i]) + 1;
    }
    s->argv[argc] = NULL;
    t->arg = s;
    return t->id;
}
