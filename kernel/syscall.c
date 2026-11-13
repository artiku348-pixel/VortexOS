/* System call dispatcher (int $0x80 from ring 3). See sdk/include/vortex/abi.h.
 * Every pointer argument is checked against the caller's address space. */
#include "kernel.h"

static int sys_fs_read(const char *path, u8 *buf, u32 max, u32 offset)
{
    struct fs_node *n = fs_lookup(path);
    if (!n || n->is_dir || fs_open(n)) return -1;
    if (offset >= n->size) return 0;
    if (max > n->size - offset) max = n->size - offset;
    memcpy(buf, n->data + offset, max);
    return (int)max;
}

static int sys_fs_stat(const char *path, struct vx_stat *st)
{
    struct fs_node *n = fs_lookup(path);
    if (!n) return -1;
    st->size = n->size;
    st->is_dir = n->is_dir;
    return 0;
}

static int sys_fs_readdir(const char *path, int index, char *name)
{
    struct fs_node *n = fs_lookup(path);
    if (!n || !n->is_dir || index < 0 || fs_open(n)) return -1;
    for (n = n->children; n && index; n = n->next) index--;
    if (!n) return 0;
    strcpy(name, n->name);
    return n->is_dir ? 2 : 1;
}

static int sys_spawn(const char *name, int argc, char **argv)
{
    char *self[2] = { (char *)name, NULL };
    struct fs_node *n = exec_find(name);
    if (!n) return -1;
    return argc > 0 ? exec_spawn(n, argc, argv) : exec_spawn(n, 1, self);
}

static int sys_wait(int id)
{
    while (task_alive(id)) task_yield();
    return 0;
}

static int sys_win_event(int me, int win, struct vx_event *ev, int timeout_ms)
{
    u32 start = timer_ticks();
    u32 limit = timeout_ms < 0 ? 0 : ((u32)timeout_ms * TIMER_HZ + 999) / 1000;
    for (;;) {
        int r = wm_event(me, win, ev);
        if (r) return r;                            /* event (1) or bad window (-1) */
        if (timeout_ms >= 0 && timer_ticks() - start >= limit) return 0;
        task_yield();
    }
}

static i64 sys_sbrk(struct task *t, u64 bytes)
{
    u64 old = t->brk;
    if (bytes > 256ul * 1024 * 1024 || vmm_alloc(t->pagedir, old, bytes)) return -1;
    t->brk = old + bytes;
    return (i64)old;
}

/* Pointer checks: user programs may only hand us memory they own. */
#define STR(p)     do { if (!user_str((const char *)(p), VX_PATH_MAX)) goto bad; } while (0)
#define TEXT(p)    do { if (!user_str((const char *)(p), 4096)) goto bad; } while (0)
#define BUF(p, n)  do { if (!user_ok((const void *)(p), (n))) goto bad; } while (0)

void syscall_dispatch(struct regs *r)
{
    struct task *t = task_current();
    /* Number in rax; arguments in rdi, rsi, rdx, r10, r8; result in rax. */
    u64 a = r->rdi, b = r->rsi, c = r->rdx, d = r->r10, e = r->r8;
    i64 ret = -1;

    if ((r->cs & 3) != 3 || !t->pagedir) goto bad;  /* system calls come from user mode */

    switch (r->rax) {
    case VX_SYS_EXIT:   task_exit((int)a);
    case VX_SYS_WRITE:
        BUF(a, b);
        for (u64 i = 0; i < b; i++) kputc(((const char *)a)[i]);
        ret = (int)b;
        break;
    case VX_SYS_GETKEY: ret = console_getchar(); break;
    case VX_SYS_SLEEP:  sleep_ms((u32)a); ret = 0; break;
    case VX_SYS_TICKS:  ret = (int)timer_ticks(); break;
    case VX_SYS_TIME:   BUF(a, sizeof(struct vx_time)); rtc_read((struct vx_time *)a); ret = 0; break;
    case VX_SYS_YIELD:  task_yield(); ret = 0; break;
    case VX_SYS_COLOR:  console_color((u8)a, (u8)b); ret = 0; break;
    case VX_SYS_CLEAR:  console_clear(); ret = 0; break;
    case VX_SYS_SBRK:   ret = sys_sbrk(t, a); break;
    case VX_SYS_SPAWN:
        STR(a);
        if ((int)b < 0 || b > 16) goto bad;
        BUF(c, b * sizeof(char *));
        for (u64 i = 0; i < b; i++) STR(((char **)c)[i]);
        ret = sys_spawn((const char *)a, (int)b, (char **)c);
        break;
    case VX_SYS_WAIT:   ret = sys_wait((int)a); break;
    case VX_SYS_GETENV: {
        STR(a); BUF(b, c);
        const char *v = env_get((const char *)a);
        if (v) { strlcpy((char *)b, v, c); ret = (int)strlen(v); }
        break;
    }
    case VX_SYS_SETENV: STR(a); STR(b); ret = env_set((const char *)a, (const char *)b); break;
    case VX_SYS_GETCWD: BUF(a, b); strlcpy((char *)a, t->cwd, b); ret = 0; break;
    case VX_SYS_CHDIR: {
        STR(a);
        struct fs_node *n = fs_lookup((const char *)a);
        if (n && n->is_dir) { fs_abspath(n, t->cwd, sizeof(t->cwd)); ret = 0; }
        break;
    }

    case VX_SYS_FS_READ:    STR(a); BUF(b, c); ret = sys_fs_read((const char *)a, (u8 *)b, c, d); break;
    case VX_SYS_FS_WRITE:   STR(a); BUF(b, c); ret = fs_write(fs_create((const char *)a, 0), (const void *)b, c, (int)d); break;
    case VX_SYS_FS_STAT:    STR(a); BUF(b, sizeof(struct vx_stat)); ret = sys_fs_stat((const char *)a, (struct vx_stat *)b); break;
    case VX_SYS_FS_READDIR: STR(a); BUF(c, VX_NAME_MAX); ret = sys_fs_readdir((const char *)a, (int)b, (char *)c); break;
    case VX_SYS_FS_MKDIR:   STR(a); ret = fs_create((const char *)a, 1) ? 0 : -1; break;
    case VX_SYS_FS_REMOVE:  STR(a); ret = fs_remove((const char *)a); break;

    case VX_SYS_WIN_CREATE:  STR(a); ret = wm_create(t->id, (const char *)a, (int)b, (int)c); break;
    case VX_SYS_WIN_DESTROY: ret = wm_destroy(t->id, (int)a); break;
    case VX_SYS_WIN_FILL:    BUF(b, sizeof(struct vx_rect)); ret = wm_fill(t->id, (int)a, (const struct vx_rect *)b, c); break;
    case VX_SYS_WIN_TEXT:    TEXT(d); ret = wm_text(t->id, (int)a, (int)b, (int)c, (const char *)d, e); break;
    case VX_SYS_WIN_BLIT: {
        BUF(b, sizeof(struct vx_rect));
        const struct vx_rect *rc = (const struct vx_rect *)b;
        if (rc->w <= 0 || rc->h <= 0 || rc->w > 4096 || rc->h > 4096) goto bad;
        BUF(c, (u64)(rc->w * rc->h) * sizeof(u32));
        ret = wm_blit(t->id, (int)a, rc, (const u32 *)c);
        break;
    }
    case VX_SYS_WIN_EVENT:   BUF(b, sizeof(struct vx_event)); ret = sys_win_event(t->id, (int)a, (struct vx_event *)b, (int)c); break;
    }
bad:
    r->rax = (u64)ret;
}
