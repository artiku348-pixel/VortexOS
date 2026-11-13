/* Flat 4 GiB segmentation: kernel code/data (ring 0), user code/data
 * (ring 3) and the TSS. Memory protection is done by paging, not segments. */
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
    u32 base;
} __attribute__((packed));

void gdt_flush(const struct table_ptr *p);
void tss_flush(void);

/* 32-bit Task State Segment. Only ss0/esp0 are used: the stack the CPU
 * switches to when an interrupt or system call arrives in ring 3. */
struct tss {
    u32 prev, esp0, ss0;
    u32 unused[22];
    u16 trap, iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[6];
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

void tss_set_kernel_stack(u32 esp0) { tss.esp0 = esp0; }

void gdt_init(void)
{
    set_gate(0, 0, 0, 0, 0);
    set_gate(1, 0, 0xFFFFF, 0x9A, 0xC);     /* 0x08 ring 0 code, 32-bit, 4K gran */
    set_gate(2, 0, 0xFFFFF, 0x92, 0xC);     /* 0x10 ring 0 data */
    set_gate(3, 0, 0xFFFFF, 0xFA, 0xC);     /* 0x18 ring 3 code */
    set_gate(4, 0, 0xFFFFF, 0xF2, 0xC);     /* 0x20 ring 3 data */
    set_gate(5, (u32)&tss, sizeof(tss) - 1, 0x89, 0x0);     /* 0x28 TSS */
    tss.ss0 = SEL_KDATA;
    tss.iomap_base = sizeof(tss);           /* no I/O bitmap: ports are ring 0 only */
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (u32)gdt;
    gdt_flush(&gdt_ptr);
    tss_flush();
}
