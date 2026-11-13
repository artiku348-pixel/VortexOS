/* Physical memory manager: a bitmap allocator for 4 KiB page frames.
 * It owns the RAM that is left after the kernel image, the initrd and the
 * kernel heap; frames are used for page tables and user-space memory. */
#include "kernel.h"

#define MAX_FRAMES (KERNEL_LIMIT / PAGE_SIZE)       /* the kernel manages at most 3 GiB */

static u32 bitmap[MAX_FRAMES / 32];                 /* 1 = used */
static u64 first, last;                             /* managed frame numbers [first, last) */
static u64 free_frames, hint;

void pmm_init(u64 start, u64 end)
{
    memset(bitmap, 0xFF, sizeof(bitmap));
    first = (start + PAGE_SIZE - 1) / PAGE_SIZE;
    last = end / PAGE_SIZE;
    if (last > MAX_FRAMES) last = MAX_FRAMES;
    for (u64 f = first; f < last; f++) bitmap[f / 32] &= ~(1u << (f % 32));
    free_frames = last > first ? last - first : 0;
    hint = first;
}

/* Returns the physical address of a zeroed frame, or 0 when out of memory. */
u64 pmm_alloc(void)
{
    if (!free_frames) return 0;
    for (u64 n = 0; n < last - first; n++) {
        u64 f = hint + n;
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

void pmm_free(u64 addr)
{
    u64 f = addr / PAGE_SIZE;
    if (f < first || f >= last || !(bitmap[f / 32] & (1u << (f % 32)))) return;
    bitmap[f / 32] &= ~(1u << (f % 32));
    free_frames++;
}

void pmm_stats(u32 *total, u32 *free)
{
    *total = (u32)(last > first ? last - first : 0);
    *free = (u32)free_frames;
}
