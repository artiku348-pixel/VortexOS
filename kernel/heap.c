/* First-fit kernel heap placed right after the kernel image and initrd. */
#include "kernel.h"

struct block {
    u32 size;                           /* payload bytes */
    u32 free;
    struct block *next;
};

#define ALIGN8(x)    (((x) + 7u) & ~7u)
#define HDR          ALIGN8(sizeof(struct block))
#define HEAP_MAX     (64u * 1024 * 1024)
#define HEAP_DEFAULT (1u * 1024 * 1024)

static struct block *heap;
static u32 heap_size;

/* `start` is the first free address after the kernel and boot modules. */
void heap_init(u32 start, u32 mem_upper_kb)
{
    start = (start + 0xFFF) & ~0xFFFu;
    u32 size = HEAP_DEFAULT;

    if (mem_upper_kb) {                 /* RAM above 1 MiB, as reported by the bootloader */
        u32 ram_end = 0x100000 + mem_upper_kb * 1024;
        if (ram_end < start + HEAP_DEFAULT) panic("not enough memory for the heap");
        size = ram_end - start;
        if (size > HEAP_MAX) size = HEAP_MAX;
    }
    heap = (struct block *)start;
    heap->size = size - HDR;
    heap->free = 1;
    heap->next = NULL;
    heap_size = size;
}

void *kmalloc(size_t size)
{
    if (!size) return NULL;
    size = ALIGN8(size);
    for (struct block *b = heap; b; b = b->next) {
        if (!b->free || b->size < size) continue;
        if (b->size >= size + HDR + 8) {            /* split the remainder off */
            struct block *rest = (struct block *)((u8 *)b + HDR + size);
            rest->size = b->size - size - HDR;
            rest->free = 1;
            rest->next = b->next;
            b->size = size;
            b->next = rest;
        }
        b->free = 0;
        return (u8 *)b + HDR;
    }
    return NULL;
}

void kfree(void *ptr)
{
    if (!ptr) return;
    ((struct block *)((u8 *)ptr - HDR))->free = 1;
    for (struct block *b = heap; b; b = b->next)    /* merge free neighbours */
        while (b->free && b->next && b->next->free) {
            b->size += HDR + b->next->size;
            b->next = b->next->next;
        }
}

void heap_stats(u32 *total, u32 *used, u32 *blocks)
{
    *total = heap_size;
    *used = *blocks = 0;
    for (struct block *b = heap; b; b = b->next) {
        (*blocks)++;
        if (!b->free) *used += b->size;
    }
}
