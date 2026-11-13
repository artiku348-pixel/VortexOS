/* System call dispatcher (int $0x80). See sdk/include/vortex/abi.h.
 * Applications currently run in ring 0, so pointers are trusted. */
#include "kernel.h"

static int sys_fs_read(const char *path, u8 *buf, u32 max, u32 offset)
{
    struct fs_node *n = fs_lookup(path);
    if (!n || n->is_dir) return -1;
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
    if (!n || !n->is_dir || index < 0) return -1;
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

void syscall_dispatch(struct regs *r)
{
    struct task *t = task_current();
    u32 a = r->ebx, b = r->ecx, c = r->edx, d = r->esi, e = r->edi;
    int ret = -1;

    switch (r->eax) {
    case VX_SYS_EXIT:   task_exit((int)a);
    case VX_SYS_WRITE:
        for (u32 i = 0; i < b; i++) kputc(((const char *)a)[i]);
        ret = (int)b;
        break;
    case VX_SYS_GETKEY: ret = console_getchar(); break;
    case VX_SYS_SLEEP:  sleep_ms(a); ret = 0; break;
    case VX_SYS_TICKS:  ret = (int)timer_ticks(); break;
    case VX_SYS_TIME:   rtc_read((struct vx_time *)a); ret = 0; break;
    case VX_SYS_YIELD:  task_yield(); ret = 0; break;
    case VX_SYS_COLOR:  console_color((u8)a, (u8)b); ret = 0; break;
    case VX_SYS_CLEAR:  console_clear(); ret = 0; break;
    case VX_SYS_ALLOC:  ret = (int)task_alloc(t, a); break;
    case VX_SYS_FREE:   task_free(t, (void *)a); ret = 0; break;
    case VX_SYS_SPAWN:  ret = sys_spawn((const char *)a, (int)b, (char **)c); break;
    case VX_SYS_WAIT:   ret = sys_wait((int)a); break;
    case VX_SYS_GETENV: {
        const char *v = env_get((const char *)a);
        if (v) { strlcpy((char *)b, v, c); ret = (int)strlen(v); }
        break;
    }
    case VX_SYS_SETENV: ret = env_set((const char *)a, (const char *)b); break;
    case VX_SYS_GETCWD: strlcpy((char *)a, t->cwd, b); ret = 0; break;
    case VX_SYS_CHDIR: {
        struct fs_node *n = fs_lookup((const char *)a);
        if (n && n->is_dir) { fs_abspath(n, t->cwd, sizeof(t->cwd)); ret = 0; }
        break;
    }

    case VX_SYS_FS_READ:    ret = sys_fs_read((const char *)a, (u8 *)b, c, d); break;
    case VX_SYS_FS_WRITE:   ret = fs_write(fs_create((const char *)a, 0), (const void *)b, c, (int)d); break;
    case VX_SYS_FS_STAT:    ret = sys_fs_stat((const char *)a, (struct vx_stat *)b); break;
    case VX_SYS_FS_READDIR: ret = sys_fs_readdir((const char *)a, (int)b, (char *)c); break;
    case VX_SYS_FS_MKDIR:   ret = fs_create((const char *)a, 1) ? 0 : -1; break;
    case VX_SYS_FS_REMOVE:  ret = fs_remove((const char *)a); break;

    case VX_SYS_WIN_CREATE:  ret = wm_create(t->id, (const char *)a, (int)b, (int)c); break;
    case VX_SYS_WIN_DESTROY: ret = wm_destroy(t->id, (int)a); break;
    case VX_SYS_WIN_FILL:    ret = wm_fill(t->id, (int)a, (const struct vx_rect *)b, c); break;
    case VX_SYS_WIN_TEXT:    ret = wm_text(t->id, (int)a, (int)b, (int)c, (const char *)d, e); break;
    case VX_SYS_WIN_BLIT:    ret = wm_blit(t->id, (int)a, (const struct vx_rect *)b, (const u32 *)c); break;
    case VX_SYS_WIN_EVENT:   ret = sys_win_event(t->id, (int)a, (struct vx_event *)b, (int)c); break;
    }
    r->eax = (u32)ret;
}
