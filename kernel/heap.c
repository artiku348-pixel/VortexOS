/* First-fit kernel heap placed right after the kernel image and initrd. */
#include "kernel.h"

struct block {
    u64 size;                           /* payload bytes */
    u64 free;
    struct block *next;
    u64 pad;                            /* keeps payloads 16-byte aligned */
};

#define ALIGN8(x)    (((x) + 15ul) & ~15ul)
#define HDR          ALIGN8(sizeof(struct block))

static struct block *heap;
static u64 heap_size;

void heap_init(u64 start, u64 size)
{
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
        if (b->size >= size + HDR + 16) {            /* split the remainder off */
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
    u64 sum = 0;
    *total = (u32)(heap_size / 1024);
    *blocks = 0;
    for (struct block *b = heap; b; b = b->next) {
        (*blocks)++;
        if (!b->free) sum += b->size;
    }
    *used = (u32)(sum / 1024);
}
