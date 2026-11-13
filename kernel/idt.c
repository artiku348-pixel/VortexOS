/* IDT, 8259 PIC remap, exception reporting and IRQ dispatch. */
#include "kernel.h"

struct idt_entry {
    u16 off_low;
    u16 selector;
    u8  zero;
    u8  type;
    u16 off_high;
} __attribute__((packed));

struct table_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

void idt_flush(const struct table_ptr *p);
extern const u32 isr_stub_table[48];
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

        if (t && t->id > 1) {           /* a faulting task dies; the system lives */
            t->cap = NULL;
            console_color(VX_LIGHT_RED, VX_BLACK);
            kprintf("\n[%d] %s crashed: %s (EIP=%08x)\n", t->id, t->name,
                    name ? name : "Reserved", r->eip);
            console_color(VX_LIGHT_GREY, VX_BLACK);
            task_exit(-1);
        }

        cpu_cli();
        if (t) { t->cap = NULL; t->con = &kcon; }
        console_emergency();
        console_color(VX_WHITE, VX_RED);
        kprintf("\n*** CPU EXCEPTION %u: %s (err=%x) ***\n",
                r->int_no, name ? name : "Reserved", r->err_code);
        kprintf("EIP=%08x CS=%04x EFLAGS=%08x\n", r->eip, r->cs, r->eflags);
        kprintf("EAX=%08x EBX=%08x ECX=%08x EDX=%08x\n", r->eax, r->ebx, r->ecx, r->edx);
        kprintf("ESI=%08x EDI=%08x EBP=%08x\n", r->esi, r->edi, r->ebp);
        kputs("System halted.\n");
        for (;;) cpu_hlt();
    }

    u32 irq = r->int_no - 32;
    if (irq < 16) {
        if (irq_handlers[irq]) irq_handlers[irq](r);
        if (irq >= 8) outb(PIC2, 0x20); /* EOI */
        outb(PIC1, 0x20);
    }
}

static void set_gate(int i, u32 addr, u8 type)
{
    idt[i].off_low  = (u16)addr;
    idt[i].selector = 0x08;
    idt[i].zero     = 0;
    idt[i].type     = type;
    idt[i].off_high = (u16)(addr >> 16);
}

void idt_init(void)
{
    for (int i = 0; i < 48; i++)        /* present, ring 0, 32-bit interrupt gate */
        set_gate(i, isr_stub_table[i], 0x8E);
    /* System calls use a trap gate: interrupts stay enabled so that a
     * blocking call can wait for the timer and the keyboard. */
    set_gate(128, (u32)isr_stub_syscall, 0x8F);
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (u32)idt;
    idt_flush(&idt_ptr);
    pic_remap();
}
