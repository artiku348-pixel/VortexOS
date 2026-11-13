/* Virtual memory: x86-64 four-level paging (PML4 -> PDPT -> PD -> PT).
 *
 * Address space layout
 *   0x0000000000001000 .. 4 GiB      kernel: RAM, framebuffer and other
 *                                    devices, identity-mapped, ring 0 only
 *   0x0000008000000000 .. +512 GiB   user: program image, heap, stack
 *
 * The kernel half is PML4 entry 0 and is shared by every address space (the
 * same lower-level tables); the user half is PML4 entry 1 and belongs to one
 * process. Page 0 is left unmapped so that NULL dereferences fault. */
#include "kernel.h"

#define PG_PRESENT 0x001ul
#define PG_WRITE   0x002ul
#define PG_USER    0x004ul
#define PG_LARGE   0x080ul              /* 2 MiB page (in a page directory) */
#define FRAME(x)   ((x) & 0x000FFFFFFFFFF000ul)
#define INDEX(virt, level) (((virt) >> (12 + 9 * (level))) & 0x1FF)
#define USER_SLOT  INDEX(USER_BASE, 3)  /* PML4 entry of user space */

static u64 *kspace;                     /* kernel address space (PML4) */
static u64 *current;

static inline void load_cr3(u64 *space)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(space) : "memory");
}

/* Page table entry (4 KiB level) for `virt`; missing tables are created
 * when `create` is set. NULL if the address is not mapped by 4 KiB pages. */
static u64 *entry(u64 *space, u64 virt, int create, u64 flags)
{
    u64 *table = space;
    for (int level = 3; level > 0; level--) {
        u64 *e = &table[INDEX(virt, level)];
        if (!(*e & PG_PRESENT)) {
            if (!create) return NULL;
            u64 frame = pmm_alloc();
            if (!frame) return NULL;
            *e = frame | PG_PRESENT | PG_WRITE | (flags & PG_USER);
        } else if (*e & PG_LARGE) {
            return NULL;
        }
        table = (u64 *)FRAME(*e);
    }
    return &table[INDEX(virt, 0)];
}

static int map(u64 *space, u64 virt, u64 phys, u64 flags)
{
    u64 *e = entry(space, virt, 1, flags);
    if (!e) return -1;
    *e = FRAME(phys) | flags | PG_PRESENT;
    return 0;
}

/* The entry that maps `virt` if it is present, else 0. */
static u64 lookup(u64 *space, u64 virt)
{
    u64 *e = entry(space, virt, 0, 0);
    return e && (*e & PG_PRESENT) ? *e : 0;
}

/* Replace the boot page tables: the first 2 MiB get 4 KiB pages (without
 * page 0), the rest of the first 4 GiB is mapped with 2 MiB pages. */
void vmm_init(void)
{
    kspace = (u64 *)pmm_alloc();
    u64 *pdpt = (u64 *)pmm_alloc();
    if (!kspace || !pdpt) panic("no memory for the kernel page tables");
    kspace[0] = (u64)pdpt | PG_PRESENT | PG_WRITE;

    for (u64 gb = 0; gb < KERNEL_MAP >> 30; gb++) {
        u64 *pd = (u64 *)pmm_alloc();
        if (!pd) panic("no memory for the kernel page tables");
        pdpt[gb] = (u64)pd | PG_PRESENT | PG_WRITE;
        for (u64 i = 0; i < 512; i++)
            pd[i] = ((gb << 30) + (i << 21)) | PG_PRESENT | PG_WRITE | PG_LARGE;
    }
    ((u64 *)FRAME(pdpt[0]))[0] = 0;                 /* 0 .. 2 MiB: small pages */
    for (u64 addr = PAGE_SIZE; addr < 0x200000; addr += PAGE_SIZE)
        if (map(kspace, addr, addr, PG_WRITE)) panic("no memory for the kernel page tables");

    current = kspace;
    load_cr3(kspace);
}

void vmm_switch(u64 *space)
{
    if (!space) space = kspace;
    if (space == current) return;
    current = space;
    load_cr3(space);
}

/* A new address space: shares the kernel mappings, has no user pages yet. */
u64 *vmm_new_space(void)
{
    u64 *space = (u64 *)pmm_alloc();
    if (space) space[0] = kspace[0];
    return space;
}

static void free_tables(u64 *table, int level)
{
    for (int i = 0; i < 512; i++) {
        if (!(table[i] & PG_PRESENT)) continue;
        if (level > 0) free_tables((u64 *)FRAME(table[i]), level - 1);
        else pmm_free(FRAME(table[i]));
    }
    pmm_free((u64)table);
}

void vmm_free_space(u64 *space)
{
    if (!space || space == kspace) return;
    if (space == current) vmm_switch(kspace);
    if (space[USER_SLOT] & PG_PRESENT) free_tables((u64 *)FRAME(space[USER_SLOT]), 2);
    pmm_free((u64)space);
}

/* Back [virt, virt+size) with fresh zeroed user pages. */
int vmm_alloc(u64 *space, u64 virt, u64 size)
{
    u64 end = virt + size;
    if (end < virt || virt < USER_BASE || end > USER_TOP) return -1;
    for (u64 page = FRAME(virt); page < end; page += PAGE_SIZE) {
        if (lookup(space, page)) continue;
        u64 frame = pmm_alloc();
        if (!frame) return -1;
        if (map(space, page, frame, PG_WRITE | PG_USER)) { pmm_free(frame); return -1; }
    }
    if (space == current) load_cr3(space);          /* flush the TLB */
    return 0;
}

/* Copy into another address space (through the identity-mapped frames). */
int vmm_copy_to(u64 *space, u64 virt, const void *src, u64 len)
{
    const u8 *s = src;
    while (len) {
        u64 pte = lookup(space, virt);
        u64 off = virt & 0xFFF, n = PAGE_SIZE - off;
        if (!pte) return -1;
        if (n > len) n = len;
        memcpy((u8 *)FRAME(pte) + off, s, n);
        virt += n; s += n; len -= n;
    }
    return 0;
}

/* ---- validation of pointers passed to system calls --------------------- */

static int user_page(u64 addr)
{
    struct task *t = task_current();
    if (!t || !t->pagedir || addr < USER_BASE || addr >= USER_TOP) return 0;
    return (lookup(t->pagedir, addr) & PG_USER) != 0;
}

int user_ok(const void *ptr, u64 len)
{
    u64 start = (u64)ptr, end = start + len;
    if (end < start) return 0;
    if (!len) return start >= USER_BASE && start < USER_TOP;
    for (u64 page = FRAME(start); page < end; page += PAGE_SIZE)
        if (!user_page(page)) return 0;
    return 1;
}

/* A NUL-terminated user string shorter than `max` bytes. */
int user_str(const char *s, u64 max)
{
    u64 addr = (u64)s;
    for (u64 n = 0; n < max; n++, addr++) {
        if ((n == 0 || !(addr & 0xFFF)) && !user_page(addr)) return 0;
        if (!*(const char *)addr) return 1;
    }
    return 0;
}
