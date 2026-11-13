/* Virtual memory: 32-bit two-level paging.
 *
 * Address space layout
 *   0x00001000 .. RAM end (max 1 GiB)  kernel: identity-mapped, supervisor only
 *   0x40000000 .. 0x80000000           user: program image, heap, stack
 *   framebuffer (PCI MMIO, >= 2 GiB)   identity-mapped, supervisor only
 *
 * Page 0 is left unmapped so that NULL dereferences fault. Every process has
 * its own page directory; the kernel part is shared (same page tables). */
#include "kernel.h"

#define PG_PRESENT 1u
#define PG_WRITE   2u
#define PG_USER    4u
#define FRAME(x)   ((x) & ~0xFFFu)

static u32 *kdir;                       /* kernel page directory */
static u32 *curdir;

static inline void load_cr3(u32 *dir)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(dir) : "memory");
}

u32 *vmm_kernel_dir(void) { return kdir; }

static int map(u32 *dir, u32 virt, u32 phys, u32 flags)
{
    u32 *pde = &dir[virt >> 22];
    if (!(*pde & PG_PRESENT)) {
        u32 table = pmm_alloc();
        if (!table) return -1;
        *pde = table | PG_PRESENT | PG_WRITE | (flags & PG_USER);
    }
    ((u32 *)FRAME(*pde))[(virt >> 12) & 0x3FF] = FRAME(phys) | flags | PG_PRESENT;
    return 0;
}

/* Page table entry for `virt`, or 0 if it is not mapped. */
static u32 lookup(const u32 *dir, u32 virt)
{
    u32 pde = dir[virt >> 22];
    if (!(pde & PG_PRESENT)) return 0;
    u32 pte = ((const u32 *)FRAME(pde))[(virt >> 12) & 0x3FF];
    return (pte & PG_PRESENT) ? pte : 0;
}

void vmm_init(u32 ram_end, u32 framebuffer, u32 framebuffer_size)
{
    kdir = (u32 *)pmm_alloc();
    if (!kdir) panic("no memory for the kernel page directory");

    for (u32 addr = PAGE_SIZE; addr < ram_end; addr += PAGE_SIZE)
        if (map(kdir, addr, addr, PG_WRITE)) panic("no memory for kernel page tables");
    for (u32 off = 0; framebuffer && off < framebuffer_size; off += PAGE_SIZE)
        if (map(kdir, framebuffer + off, framebuffer + off, PG_WRITE))
            panic("no memory for framebuffer page tables");

    curdir = kdir;
    load_cr3(kdir);
    u32 cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u | 0x00010000u;   /* PG: paging on, WP: honour read-only in ring 0 */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

void vmm_switch(u32 *dir)
{
    if (!dir) dir = kdir;
    if (dir == curdir) return;
    curdir = dir;
    load_cr3(dir);
}

/* A new address space: shares the kernel mappings, has no user pages yet. */
u32 *vmm_new_dir(void)
{
    u32 *dir = (u32 *)pmm_alloc();
    if (dir) memcpy(dir, kdir, PAGE_SIZE);
    return dir;
}

void vmm_free_dir(u32 *dir)
{
    if (!dir || dir == kdir) return;
    if (dir == curdir) vmm_switch(kdir);
    for (u32 i = USER_BASE >> 22; i < USER_TOP >> 22; i++) {
        if (!(dir[i] & PG_PRESENT)) continue;
        u32 *table = (u32 *)FRAME(dir[i]);
        for (int k = 0; k < 1024; k++)
            if (table[k] & PG_PRESENT) pmm_free(FRAME(table[k]));
        pmm_free((u32)table);
    }
    pmm_free((u32)dir);
}

/* Back [virt, virt+size) with fresh zeroed user pages. */
int vmm_alloc(u32 *dir, u32 virt, u32 size)
{
    u32 end = virt + size;
    if (end < virt || virt < USER_BASE || end > USER_TOP) return -1;
    for (u32 page = FRAME(virt); page < end; page += PAGE_SIZE) {
        if (lookup(dir, page)) continue;
        u32 frame = pmm_alloc();
        if (!frame) return -1;
        if (map(dir, page, frame, PG_WRITE | PG_USER)) { pmm_free(frame); return -1; }
    }
    if (dir == curdir) load_cr3(dir);   /* flush the TLB */
    return 0;
}

/* Copy into another address space (through the identity-mapped frames). */
int vmm_copy_to(u32 *dir, u32 virt, const void *src, u32 len)
{
    const u8 *s = src;
    while (len) {
        u32 pte = lookup(dir, virt);
        u32 off = virt & 0xFFF, n = PAGE_SIZE - off;
        if (!pte) return -1;
        if (n > len) n = len;
        memcpy((u8 *)FRAME(pte) + off, s, n);
        virt += n; s += n; len -= n;
    }
    return 0;
}

/* ---- validation of pointers passed to system calls --------------------- */

static int user_page(u32 addr)
{
    struct task *t = task_current();
    if (!t || !t->pagedir || addr < USER_BASE || addr >= USER_TOP) return 0;
    return (lookup(t->pagedir, addr) & PG_USER) != 0;
}

int user_ok(const void *ptr, u32 len)
{
    u32 start = (u32)ptr, end = start + len;
    if (end < start) return 0;
    if (!len) return start >= USER_BASE && start < USER_TOP;
    for (u32 page = FRAME(start); page < end; page += PAGE_SIZE)
        if (!user_page(page)) return 0;
    return 1;
}

/* A NUL-terminated user string shorter than `max` bytes. */
int user_str(const char *s, u32 max)
{
    u32 addr = (u32)s;
    for (u32 n = 0; n < max; n++, addr++) {
        if ((n == 0 || !(addr & 0xFFF)) && !user_page(addr)) return 0;
        if (!*(const char *)addr) return 1;
    }
    return 0;
}
