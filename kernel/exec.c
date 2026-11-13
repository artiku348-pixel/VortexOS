/* Program loader: runs position-independent ELF32 executables (built with
 * the SDK) as user processes: own address space, ring 3, own stack. */
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

#define USER_STACK_SIZE (64u * 1024)
#define ARG_BYTES_MAX   2048u

struct app_start { u32 entry, esp; };

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
    enter_user(s->entry, s->esp);
}

/* Map the program into the address space `dir` at USER_BASE.
 * Returns the end of the image (page aligned) or 0; `*entry` gets the entry point. */
static u32 load_image(u32 *dir, const struct fs_node *n, u32 *entry)
{
    const struct elf_hdr *eh = (const struct elf_hdr *)n->data;
    if (eh->ident[4] != 1 || eh->type != ET_DYN || eh->machine != EM_386) return 0;
    if (eh->phentsize != sizeof(struct elf_phdr) || eh->phnum > 32 ||
        eh->phoff > n->size || eh->phoff + eh->phnum * sizeof(struct elf_phdr) > n->size)
        return 0;

    const struct elf_phdr *ph = (const struct elf_phdr *)(n->data + eh->phoff);
    const struct elf_phdr *dynamic = NULL;
    u32 lo = 0xFFFFFFFF, hi = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == PT_DYNAMIC) dynamic = &ph[i];
        if (ph[i].type != PT_LOAD) continue;
        if (ph[i].filesz > ph[i].memsz || ph[i].offset > n->size ||
            ph[i].filesz > n->size - ph[i].offset || ph[i].memsz > 0x4000000 ||
            ph[i].vaddr > 0x4000000)
            return 0;
        if (ph[i].vaddr < lo) lo = ph[i].vaddr;
        if (ph[i].vaddr + ph[i].memsz > hi) hi = ph[i].vaddr + ph[i].memsz;
    }
    if (hi <= lo || eh->entry < lo || eh->entry >= hi) return 0;

    /* Build the relocated image in kernel memory, then copy it to user pages. */
    u32 span = hi - lo;
    u8 *img = kmalloc(span);
    if (!img) return 0;
    u32 run = USER_BASE - lo;           /* run address  = link address + run */
    u8 *at = img - lo;                  /* kernel copy  = link address + at  */
    u32 end = 0;
    memset(img, 0, span);
    for (int i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD)
            memcpy(at + ph[i].vaddr, n->data + ph[i].offset, ph[i].filesz);

    if (dynamic) {
        u32 rel = 0, relsz = 0;
        if (dynamic->vaddr < lo || dynamic->vaddr + dynamic->memsz > hi) goto out;
        const struct elf_dyn *d = (const struct elf_dyn *)(at + dynamic->vaddr);
        for (u32 i = 0; i < dynamic->memsz / sizeof(*d) && d[i].tag != DT_NULL; i++) {
            if (d[i].tag == DT_REL) rel = d[i].val;
            if (d[i].tag == DT_RELSZ) relsz = d[i].val;
        }
        if (relsz && (rel < lo || rel + relsz > hi)) goto out;
        const struct elf_rel *r = (const struct elf_rel *)(at + rel);
        for (u32 i = 0; i < relsz / sizeof(*r); i++) {
            u32 type = r[i].info & 0xFF;
            if (type == R_386_NONE) continue;
            if (type != R_386_RELATIVE || r[i].offset < lo || r[i].offset + 4 > hi) goto out;
            *(u32 *)(at + r[i].offset) += run;
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

/* Put argc/argv on the new user stack; returns the initial stack pointer. */
static u32 setup_stack(u32 *dir, int argc, char **argv)
{
    u8 block[ARG_BYTES_MAX];
    u32 strings = 0;

    if (argc < 0 || argc > 32) return 0;
    for (int i = 0; i < argc; i++) strings += strlen(argv[i]) + 1;
    u32 size = (3 + (u32)argc + 1) * sizeof(u32) + strings;
    size = (size + 15) & ~15u;
    if (size > sizeof(block)) return 0;
    if (vmm_alloc(dir, USER_TOP - USER_STACK_SIZE, USER_STACK_SIZE)) return 0;

    /* Layout at esp: return address (none), argc, argv, argv[0..argc], strings. */
    u32 esp = USER_TOP - size;
    u32 *words = (u32 *)block;
    u32 str = 4 * sizeof(u32) + (u32)argc * sizeof(u32);
    words[0] = 0;
    words[1] = (u32)argc;
    words[2] = esp + 3 * sizeof(u32);
    for (int i = 0; i < argc; i++) {
        words[3 + i] = esp + str;
        strcpy((char *)block + str, argv[i]);
        str += strlen(argv[i]) + 1;
    }
    words[3 + argc] = 0;
    return vmm_copy_to(dir, esp, block, size) ? 0 : esp;
}

int exec_spawn(struct fs_node *n, int argc, char **argv)
{
    if (!exec_is_elf(n)) return -1;

    struct task *t = task_create(n->name, app_main, NULL);
    if (!t) return -1;

    struct app_start *s = task_alloc(t, sizeof(*s));
    t->pagedir = vmm_new_dir();
    if (s && t->pagedir) {
        t->brk = load_image(t->pagedir, n, &s->entry);
        s->esp = setup_stack(t->pagedir, argc, argv);
    }
    if (!s || !t->pagedir || !t->brk || !s->esp) {
        task_kill(t->id);
        return -1;
    }
    t->arg = s;
    return t->id;
}
