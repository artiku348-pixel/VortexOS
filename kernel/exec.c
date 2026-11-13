/* Program loader: runs position-independent ELF64 executables (built with
 * the SDK) as user processes: own address space, ring 3, own stack. */
#include "kernel.h"

struct elf_hdr {
    u8  ident[16];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct elf_phdr { u32 type, flags; u64 offset, vaddr, paddr, filesz, memsz, align; };
struct elf_dyn  { i64 tag; u64 val; };
struct elf_rela { u64 offset, info; i64 addend; };

enum { PT_LOAD = 1, PT_DYNAMIC = 2, ET_DYN = 3, EM_X86_64 = 62, ELFCLASS64 = 2 };
enum { DT_NULL = 0, DT_RELA = 7, DT_RELASZ = 8, R_X86_64_NONE = 0, R_X86_64_RELATIVE = 8 };

#define USER_STACK_SIZE (128ul * 1024)
#define ARG_BYTES_MAX   2048u
#define IMAGE_MAX       0x4000000ul     /* 64 MiB */

struct app_start { u64 entry, rsp, argc, argv; };

int exec_is_elf(struct fs_node *n)
{
    return n && !n->is_dir && n->size >= sizeof(struct elf_hdr) && fs_open(n) == 0 &&
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

/* First code of a new process: runs in the kernel, then drops to ring 3. */
static void app_main(void *arg)
{
    struct app_start *s = arg;
    enter_user(s->entry, s->rsp, s->argc, s->argv);
}

/* Map the program into the address space `dir` at USER_BASE.
 * Returns the end of the image (page aligned) or 0; `*entry` gets the entry point. */
static u64 load_image(u64 *dir, const struct fs_node *n, u64 *entry)
{
    const struct elf_hdr *eh = (const struct elf_hdr *)n->data;
    if (eh->ident[4] != ELFCLASS64 || eh->type != ET_DYN || eh->machine != EM_X86_64) return 0;
    if (eh->phentsize != sizeof(struct elf_phdr) || eh->phnum > 32 ||
        eh->phoff > n->size || eh->phoff + eh->phnum * sizeof(struct elf_phdr) > n->size)
        return 0;

    const struct elf_phdr *ph = (const struct elf_phdr *)(n->data + eh->phoff);
    const struct elf_phdr *dynamic = NULL;
    u64 lo = ~0ul, hi = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == PT_DYNAMIC) dynamic = &ph[i];
        if (ph[i].type != PT_LOAD) continue;
        if (ph[i].filesz > ph[i].memsz || ph[i].offset > n->size ||
            ph[i].filesz > n->size - ph[i].offset || ph[i].memsz > IMAGE_MAX ||
            ph[i].vaddr > IMAGE_MAX)
            return 0;
        if (ph[i].vaddr < lo) lo = ph[i].vaddr;
        if (ph[i].vaddr + ph[i].memsz > hi) hi = ph[i].vaddr + ph[i].memsz;
    }
    if (hi <= lo || eh->entry < lo || eh->entry >= hi) return 0;

    /* Build the relocated image in kernel memory, then copy it to user pages. */
    u64 span = hi - lo;
    u8 *img = kmalloc(span);
    if (!img) return 0;
    u64 run = USER_BASE - lo;           /* run address  = link address + run */
    u8 *at = img - lo;                  /* kernel copy  = link address + at  */
    u64 end = 0;
    memset(img, 0, span);
    for (int i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD)
            memcpy(at + ph[i].vaddr, n->data + ph[i].offset, ph[i].filesz);

    if (dynamic) {
        u64 rela = 0, relasz = 0;
        if (dynamic->vaddr < lo || dynamic->vaddr + dynamic->memsz > hi) goto out;
        const struct elf_dyn *d = (const struct elf_dyn *)(at + dynamic->vaddr);
        for (u64 i = 0; i < dynamic->memsz / sizeof(*d) && d[i].tag != DT_NULL; i++) {
            if (d[i].tag == DT_RELA) rela = d[i].val;
            if (d[i].tag == DT_RELASZ) relasz = d[i].val;
        }
        if (relasz && (rela < lo || rela > hi || relasz > hi - rela)) goto out;
        const struct elf_rela *r = (const struct elf_rela *)(at + rela);
        for (u64 i = 0; i < relasz / sizeof(*r); i++) {
            u32 type = (u32)r[i].info;
            if (type == R_X86_64_NONE) continue;
            if (type != R_X86_64_RELATIVE || r[i].offset < lo || r[i].offset > hi - 8) goto out;
            *(u64 *)(at + r[i].offset) = run + (u64)r[i].addend;
        }
    }
    if (vmm_alloc(dir, USER_BASE, span) == 0 && vmm_copy_to(dir, USER_BASE, img, span) == 0) {
        *entry = eh->entry + run;
        end = (USER_BASE + span + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    }
out:
    kfree(img);
    return end;
}

/* Put the argv array and its strings at the top of the new user stack.
 * argc and argv themselves travel in registers (see enter_user). */
static int setup_stack(u64 *dir, int argc, char **argv, struct app_start *s)
{
    u8 block[ARG_BYTES_MAX];
    u64 strings = 0;

    if (argc < 0 || argc > 32) return -1;
    for (int i = 0; i < argc; i++) strings += strlen(argv[i]) + 1;
    u64 str = ((u64)argc + 1) * sizeof(u64);
    u64 size = (str + strings + 15) & ~15ul;
    if (size > sizeof(block)) return -1;
    if (vmm_alloc(dir, USER_TOP - USER_STACK_SIZE, USER_STACK_SIZE)) return -1;

    u64 base = USER_TOP - size;
    u64 *words = (u64 *)block;
    for (int i = 0; i < argc; i++) {
        words[i] = base + str;
        strcpy((char *)block + str, argv[i]);
        str += strlen(argv[i]) + 1;
    }
    words[argc] = 0;
    if (vmm_copy_to(dir, base, block, size)) return -1;
    s->argc = (u64)argc;
    s->argv = base;
    s->rsp = base - 8;                  /* as if _start had been called: rsp = 16n + 8 */
    return 0;
}

int exec_spawn(struct fs_node *n, int argc, char **argv)
{
    if (!exec_is_elf(n)) return -1;

    struct task *t = task_create(n->name, app_main, NULL);
    if (!t) return -1;

    struct app_start *s = task_alloc(t, sizeof(*s));
    t->pagedir = vmm_new_space();
    int ok = 0;
    if (s && t->pagedir) {
        t->brk = load_image(t->pagedir, n, &s->entry);
        ok = t->brk && setup_stack(t->pagedir, argc, argv, s) == 0;
    }
    if (!ok) {
        task_kill(t->id);
        return -1;
    }
    t->arg = s;
    return t->id;
}
