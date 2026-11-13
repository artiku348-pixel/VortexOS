/* Tasks and the round-robin scheduler.
 *
 * Every task has a kernel stack; user processes additionally have their own
 * address space and run in ring 3. Kernel code switches tasks only at
 * task_yield() (blocking calls yield in a loop), while user code is preempted
 * by any interrupt (see interrupt_dispatch). Task 0 is the kernel/main shell,
 * task 1 is the idle task that halts the CPU until the next interrupt. */
#include "kernel.h"

#define STACK_SIZE (64 * 1024)

struct alloc_hdr { struct alloc_hdr *prev, *next; };

void switch_context(u64 *old_rsp, u64 new_rsp);

static struct task tasks[TASK_MAX];
static struct task *cur;
static int next_id;
static volatile int skip_halt;

struct task *task_current(void) { return cur; }
struct task *task_slot(int i) { return &tasks[i]; }

static void trampoline(void)
{
    cpu_sti();
    cur->entry(cur->arg);
    task_exit(0);
}

static void idle(void *arg)
{
    (void)arg;
    for (;;) {
        if (!skip_halt) cpu_hlt();      /* sleep until the next interrupt */
        skip_halt = 0;
        task_yield();
    }
}

/* Give every task one more turn before the CPU sleeps, e.g. after input
 * was handed to another task that already had its turn this round. */
void task_poke(void) { skip_halt = 1; }

void task_init(void)
{
    cur = &tasks[0];
    cur->id = next_id++;
    cur->state = TASK_READY;
    cur->con = &kcon;
    strcpy(cur->name, "kernel");
    strcpy(cur->cwd, "/");
    if (!task_create("idle", idle, NULL)) panic("cannot create the idle task");
}

struct task *task_create(const char *name, void (*entry)(void *), void *arg)
{
    struct task *t = NULL;
    for (int i = 1; i < TASK_MAX && !t; i++)
        if (tasks[i].state == TASK_FREE) t = &tasks[i];
    if (!t) return NULL;

    void *stack = kmalloc(STACK_SIZE);
    if (!stack) return NULL;

    memset(t, 0, sizeof(*t));
    t->id = next_id++;
    t->stack = stack;
    t->entry = entry;
    t->arg = arg;
    t->con = cur->con;
    t->cap = cur->cap;
    strlcpy(t->name, name, sizeof(t->name));
    strcpy(t->cwd, cur->cwd);

    /* Frame consumed by switch_context: r15, r14, r13, r12, rbx, rbp, rflags,
     * ret. The fake return address also gives trampoline the stack alignment
     * of a normal call. */
    u64 *sp = (u64 *)((u8 *)stack + STACK_SIZE);
    *--sp = 0;                          /* fake return address of trampoline */
    *--sp = (u64)trampoline;
    *--sp = 0x002;                      /* rflags: interrupts off until trampoline */
    for (int i = 0; i < 6; i++) *--sp = 0;
    t->rsp = (u64)sp;
    t->state = TASK_READY;
    return t;
}

void task_yield(void)
{
    for (int i = 0; i < TASK_MAX; i++)  /* reap tasks that exited by themselves */
        if (tasks[i].state == TASK_ZOMBIE && &tasks[i] != cur) {
            kfree(tasks[i].stack);
            tasks[i].state = TASK_FREE;
        }

    struct task *prev = cur;
    int base = (int)(prev - tasks);
    for (int n = 1; n <= TASK_MAX; n++) {
        struct task *t = &tasks[(base + n) % TASK_MAX];
        if (t->state != TASK_READY) continue;
        if (t == prev) return;
        cur = t;
        tss_set_kernel_stack((u64)t->stack + STACK_SIZE);
        vmm_switch(t->pagedir);
        switch_context(&prev->rsp, t->rsp);
        return;
    }
}

void *task_alloc(struct task *t, size_t size)
{
    struct alloc_hdr *h = kmalloc(sizeof(*h) + size);
    if (!h) return NULL;
    h->prev = NULL;
    h->next = t->allocs;
    if (h->next) h->next->prev = h;
    t->allocs = h;
    return h + 1;
}

void task_free(struct task *t, void *ptr)
{
    if (!ptr) return;
    struct alloc_hdr *h = (struct alloc_hdr *)ptr - 1;
    if (h->prev) h->prev->next = h->next;
    else t->allocs = h->next;
    if (h->next) h->next->prev = h->prev;
    kfree(h);
}

/* Release everything a task owns except its stack. */
static void cleanup(struct task *t)
{
    wm_destroy_owned(t->id);

    struct console *own = t->own_con;
    t->own_con = NULL;
    if (own)                            /* its console dies: so do its users */
        for (int i = 2; i < TASK_MAX; i++)
            if (&tasks[i] != t && tasks[i].state == TASK_READY && tasks[i].con == own)
                task_kill(tasks[i].id);

    while (t->allocs) task_free(t, (struct alloc_hdr *)t->allocs + 1);

    u64 *dir = t->pagedir;              /* user process: release its address space */
    t->pagedir = NULL;
    if (dir) {
        if (t == cur) vmm_switch(NULL);
        vmm_free_space(dir);
    }
}

void task_exit(int code)
{
    if (cur->id == 0) panic("the kernel task exited");
    cleanup(cur);
    cur->exit_code = code;
    cur->state = TASK_ZOMBIE;
    task_yield();
    for (;;) { }
}

static struct task *find(int id)
{
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i].state == TASK_READY && tasks[i].id == id) return &tasks[i];
    return NULL;
}

int task_alive(int id) { return find(id) != NULL; }

int task_kill(int id)
{
    struct task *t = find(id);
    if (!t || id <= 1 || t->system) return -1;       /* kernel and idle tasks are immortal */
    if (t == cur) task_exit(-1);
    cleanup(t);
    kfree(t->stack);
    t->state = TASK_FREE;
    return 0;
}

int task_wait(int id)
{
    struct console *con = cur->con;
    int saved = con->fg_task, interrupted = 0;

    con->fg_task = id;
    con->interrupted = 0;
    while (task_alive(id)) {
        console_peek(con);              /* keep the keyboard flowing */
        if (con->interrupted) {         /* Ctrl+C */
            task_kill(id);
            interrupted = 1;
            break;
        }
        task_yield();
    }
    con->fg_task = saved;
    con->interrupted = 0;
    return interrupted;
}
