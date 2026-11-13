/* Long mode GDT: 64-bit kernel code/data (ring 0), user code/data (ring 3)
 * and the TSS. Segments are flat; memory protection is done by paging. */
#include "kernel.h"

struct gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  gran;
    u8  base_high;
} __attribute__((packed));

struct table_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

void gdt_flush(const struct table_ptr *p);
void tss_flush(void);

/* 64-bit Task State Segment. Only rsp0 is used: the stack the CPU switches
 * to when an interrupt or system call arrives in ring 3. */
struct tss {
    u32 reserved0;
    u64 rsp0, rsp1, rsp2;
    u64 reserved1;
    u64 ist[7];
    u64 reserved2;
    u16 reserved3, iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[7];         /* the TSS descriptor takes two slots */
static struct table_ptr gdt_ptr;
static struct tss tss;

static void set_gate(int i, u32 base, u32 limit, u8 access, u8 flags)
{
    gdt[i].limit_low = (u16)limit;
    gdt[i].base_low  = (u16)base;
    gdt[i].base_mid  = (u8)(base >> 16);
    gdt[i].access    = access;
    gdt[i].gran      = (u8)(((limit >> 16) & 0x0F) | (flags << 4));
    gdt[i].base_high = (u8)(base >> 24);
}

void tss_set_kernel_stack(u64 rsp0) { tss.rsp0 = rsp0; }

void gdt_init(void)
{
    u64 base = (u64)&tss;

    set_gate(0, 0, 0, 0, 0);
    set_gate(1, 0, 0xFFFFF, 0x9A, 0xA);     /* 0x08 ring 0 code, L = 64-bit */
    set_gate(2, 0, 0xFFFFF, 0x92, 0xC);     /* 0x10 ring 0 data */
    set_gate(3, 0, 0xFFFFF, 0xFA, 0xA);     /* 0x18 ring 3 code, 64-bit */
    set_gate(4, 0, 0xFFFFF, 0xF2, 0xC);     /* 0x20 ring 3 data */
    set_gate(5, (u32)base, sizeof(tss) - 1, 0x89, 0x0);     /* 0x28 TSS (16 bytes) */
    *(u64 *)&gdt[6] = base >> 32;           /* upper half of the TSS address */
    tss.iomap_base = sizeof(tss);           /* no I/O bitmap: ports are ring 0 only */
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (u64)gdt;
    gdt_flush(&gdt_ptr);
    tss_flush();
}
