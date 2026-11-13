/* Flat 4 GiB segmentation: null, kernel code (0x08), kernel data (0x10). */
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

static struct gdt_entry gdt[3];
static struct table_ptr gdt_ptr;

static void set_gate(int i, u32 base, u32 limit, u8 access, u8 flags)
{
    gdt[i].limit_low = (u16)limit;
    gdt[i].base_low  = (u16)base;
    gdt[i].base_mid  = (u8)(base >> 16);
    gdt[i].access    = access;
    gdt[i].gran      = (u8)(((limit >> 16) & 0x0F) | (flags << 4));
    gdt[i].base_high = (u8)(base >> 24);
}

void gdt_init(void)
{
    set_gate(0, 0, 0, 0, 0);
    set_gate(1, 0, 0xFFFFF, 0x9A, 0xC);     /* ring 0 code, 32-bit, 4K gran */
    set_gate(2, 0, 0xFFFFF, 0x92, 0xC);     /* ring 0 data */
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (u32)gdt;
    gdt_flush(&gdt_ptr);
}
