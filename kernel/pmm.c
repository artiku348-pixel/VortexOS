/* Physical memory manager: a bitmap allocator for 4 KiB page frames.
 * It owns the RAM that is left after the kernel image, the initrd and the
 * kernel heap; frames are used for page tables and user-space memory. */
#include "kernel.h"

#define MAX_FRAMES (KERNEL_LIMIT / PAGE_SIZE)       /* the kernel sees at most 1 GiB */

static u32 bitmap[MAX_FRAMES / 32];                 /* 1 = used */
static u32 first, last;                             /* managed frame numbers [first, last) */
static u32 free_frames, hint;

void pmm_init(u32 start, u32 end)
{
    memset(bitmap, 0xFF, sizeof(bitmap));
    first = (start + PAGE_SIZE - 1) / PAGE_SIZE;
    last = end / PAGE_SIZE;
    if (last > MAX_FRAMES) last = MAX_FRAMES;
    for (u32 f = first; f < last; f++) bitmap[f / 32] &= ~(1u << (f % 32));
    free_frames = last > first ? last - first : 0;
    hint = first;
}

/* Returns the physical address of a zeroed frame, or 0 when out of memory. */
u32 pmm_alloc(void)
{
    if (!free_frames) return 0;
    for (u32 n = 0; n < last - first; n++) {
        u32 f = hint + n;
        if (f >= last) f -= last - first;
        if (bitmap[f / 32] & (1u << (f % 32))) continue;
        bitmap[f / 32] |= 1u << (f % 32);
        free_frames--;
        hint = f + 1 < last ? f + 1 : first;
        memset((void *)(f * PAGE_SIZE), 0, PAGE_SIZE);   /* RAM is identity-mapped */
        return f * PAGE_SIZE;
    }
    return 0;
}

void pmm_free(u32 addr)
{
    u32 f = addr / PAGE_SIZE;
    if (f < first || f >= last || !(bitmap[f / 32] & (1u << (f % 32)))) return;
    bitmap[f / 32] &= ~(1u << (f % 32));
    free_frames++;
}

void pmm_stats(u32 *total, u32 *free)
{
    *total = last > first ? last - first : 0;
    *free = free_frames;
}
