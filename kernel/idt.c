/* IDT, 8259 PIC remap, exception reporting and IRQ dispatch. */
#include "kernel.h"

struct idt_entry {                      /* 64-bit interrupt gate: 16 bytes */
    u16 off_low;
    u16 selector;
    u8  ist;
    u8  type;
    u16 off_mid;
    u32 off_high;
    u32 zero;
} __attribute__((packed));

struct table_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

void idt_flush(const struct table_ptr *p);
extern const u64 isr_stub_table[48];
void isr_stub_syscall(void);

static struct idt_entry idt[256];
static struct table_ptr idt_ptr;
static irq_handler_t irq_handlers[16];

static const char *const exc_names[32] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor segment overrun", "Invalid TSS",
    "Segment not present", "Stack-segment fault", "General protection fault",
    "Page fault", "Reserved", "x87 FPU error", "Alignment check",
    "Machine check", "SIMD exception", "Virtualization exception",
    "Control protection exception",
};

#define PIC1 0x20
#define PIC2 0xA0

static void pic_remap(void)
{
    outb(PIC1, 0x11);     io_wait();    /* ICW1: init, expect ICW4 */
    outb(PIC2, 0x11);     io_wait();
    outb(PIC1 + 1, 0x20); io_wait();    /* ICW2: IRQ0-7  -> vectors 32-39 */
    outb(PIC2 + 1, 0x28); io_wait();    /*       IRQ8-15 -> vectors 40-47 */
    outb(PIC1 + 1, 0x04); io_wait();    /* ICW3: slave on IRQ2 */
    outb(PIC2 + 1, 0x02); io_wait();
    outb(PIC1 + 1, 0x01); io_wait();    /* ICW4: 8086 mode */
    outb(PIC2 + 1, 0x01); io_wait();
    outb(PIC1 + 1, 0xFB);               /* mask everything except cascade */
    outb(PIC2 + 1, 0xFF);
}

void irq_install(int irq, irq_handler_t handler)
{
    if (irq < 0 || irq > 15) return;
    irq_handlers[irq] = handler;
    u16 port = irq < 8 ? PIC1 + 1 : PIC2 + 1;
    outb(port, (u8)(inb(port) & ~(1 << (irq & 7))));    /* unmask */
}

/* Called from isr_common in boot/isr.S. */
void interrupt_dispatch(struct regs *r)
{
    if (r->int_no == 128) {
        syscall_dispatch(r);
        return;
    }

    if (r->int_no < 32) {
        const char *name = exc_names[r->int_no];
        struct task *t = task_current();

        u64 cr2 = 0;
        if (r->int_no == 14) __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

        if (t && t->id > 1) {           /* a faulting task dies; the system lives */
            t->cap = NULL;
            console_color(VX_LIGHT_RED, VX_BLACK);
            kprintf("\n[%d] %s crashed: %s (RIP=%016lx", t->id, t->name,
                    name ? name : "Reserved", r->rip);
            if (r->int_no == 14) kprintf(", address %016lx", cr2);
            kputs(")\n");
            console_color(VX_LIGHT_GREY, VX_BLACK);
            task_exit(-1);
        }

        cpu_cli();
        if (t) { t->cap = NULL; t->con = &kcon; }
        console_emergency();
        console_color(VX_WHITE, VX_RED);
        kprintf("\n*** CPU EXCEPTION %lu: %s (err=%lx) ***\n",
                r->int_no, name ? name : "Reserved", r->err_code);
        kprintf("RIP=%016lx CS=%04lx RFLAGS=%08lx\n", r->rip, r->cs, r->rflags);
        kprintf("RSP=%016lx CR2=%016lx\n", r->rsp, cr2);
        kprintf("RAX=%016lx RBX=%016lx\nRCX=%016lx RDX=%016lx\n", r->rax, r->rbx, r->rcx, r->rdx);
        kprintf("RSI=%016lx RDI=%016lx\nRBP=%016lx R8 =%016lx\n", r->rsi, r->rdi, r->rbp, r->r8);
        kputs("System halted.\n");
        for (;;) cpu_hlt();
    }

    u64 irq = r->int_no - 32;
    if (irq < 16) {
        if (irq_handlers[irq]) irq_handlers[irq](r);
        if (irq >= 8) outb(PIC2, 0x20); /* EOI */
        outb(PIC1, 0x20);

        /* Preemption: an interrupt that arrives while a user program runs
         * takes the CPU away from it. Kernel code is never preempted. */
        if ((r->cs & 3) == 3) {
            task_poke();
            task_yield();
        }
    }
}

static void set_gate(int i, u64 addr, u8 type)
{
    idt[i].off_low  = (u16)addr;
    idt[i].selector = SEL_KCODE;
    idt[i].ist      = 0;
    idt[i].type     = type;
    idt[i].off_mid  = (u16)(addr >> 16);
    idt[i].off_high = (u32)(addr >> 32);
    idt[i].zero     = 0;
}

void idt_init(void)
{
    for (int i = 0; i < 48; i++)        /* present, ring 0, 64-bit interrupt gate */
        set_gate(i, isr_stub_table[i], 0x8E);
    /* System calls use a trap gate: interrupts stay enabled so that a
     * blocking call can wait for the timer and the keyboard. */
    set_gate(128, (u64)isr_stub_syscall, 0xEF);  /* DPL 3: callable from user mode */
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (u64)idt;
    idt_flush(&idt_ptr);
    pic_remap();
}
