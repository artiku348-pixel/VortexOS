/* VortexOS entry point.
 *
 * A Multiboot 1 loader starts us in 32-bit protected mode. The code below
 * checks that the CPU is 64-bit capable, builds page tables that identity-map
 * the first 4 GiB, switches to long mode and calls kmain() as 64-bit code. */
.set MB_MAGIC, 0x1BADB002
.set MB_FLAGS, 0x3                      /* page-align modules + memory info */
.set MB_CHECK, -(MB_MAGIC + MB_FLAGS)

.section .multiboot, "a"
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECK

.section .bss
.align 4096
boot_pml4: .skip 4096
boot_pdpt: .skip 4096
boot_pd:   .skip 4096 * 4               /* 4 x 512 entries of 2 MiB = 4 GiB */
stack_bottom:
.skip 16384
stack_top:

.section .rodata
.align 16
boot_gdt:
    .quad 0
    .quad 0x00AF9A000000FFFF            /* 0x08: 64-bit ring 0 code */
    .quad 0x00CF92000000FFFF            /* 0x10: ring 0 data */
boot_gdt_ptr:
    .word . - boot_gdt - 1
    .long boot_gdt
no_long_mode_msg:
    .asciz "VortexOS needs a 64-bit (x86-64) processor. System halted."

/* ---- 32-bit part --------------------------------------------------------- */
.section .text
.code32
.global _start
.type _start, @function
_start:
    cli
    mov  $stack_top, %esp
    mov  %eax, %edi                     /* multiboot magic  -> kmain argument 1 */
    mov  %ebx, %esi                     /* multiboot info   -> kmain argument 2 */

    /* Is long mode available? (CPUID 0x80000001, EDX bit 29) */
    mov  $0x80000000, %eax
    cpuid
    cmp  $0x80000001, %eax
    jb   no_long_mode
    mov  $0x80000001, %eax
    cpuid
    test $(1 << 29), %edx
    jz   no_long_mode

    /* Page tables: PML4[0] -> PDPT[0..3] -> four page directories whose
     * entries are 2 MiB pages covering 0 .. 4 GiB (present, writable). */
    mov  $boot_pdpt + 0x3, %eax
    mov  %eax, boot_pml4
    mov  $boot_pd + 0x3, %eax
    xor  %ecx, %ecx
1:  mov  %eax, boot_pdpt(, %ecx, 8)
    add  $4096, %eax
    inc  %ecx
    cmp  $4, %ecx
    jne  1b
    xor  %ecx, %ecx
2:  mov  %ecx, %eax
    shl  $21, %eax
    or   $0x83, %eax                    /* present, writable, 2 MiB page */
    mov  %eax, boot_pd(, %ecx, 8)
    inc  %ecx
    cmp  $2048, %ecx
    jne  2b

    mov  $boot_pml4, %eax
    mov  %eax, %cr3
    mov  %cr4, %eax
    or   $(1 << 5), %eax                /* CR4.PAE */
    mov  %eax, %cr4
    mov  $0xC0000080, %ecx              /* EFER */
    rdmsr
    or   $(1 << 8), %eax                /* EFER.LME: long mode enable */
    wrmsr
    mov  %cr0, %eax
    or   $0x80010000, %eax              /* CR0.PG | CR0.WP: paging on = long mode active */
    mov  %eax, %cr0

    lgdt boot_gdt_ptr
    ljmp $0x08, $long_mode_entry

no_long_mode:                           /* print the message in VGA text mode */
    mov  $no_long_mode_msg, %esi
    mov  $0xB8000, %edi
    mov  $0x4F, %ah                     /* white on red */
3:  lodsb
    test %al, %al
    jz   4f
    stosw
    jmp  3b
4:  cli
    hlt
    jmp  4b

/* ---- 64-bit part --------------------------------------------------------- */
.code64
.global long_mode_entry
long_mode_entry:
    mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %ss
    xor  %eax, %eax
    mov  %ax, %fs
    mov  %ax, %gs
    mov  $stack_top, %rsp
    xor  %ebp, %ebp
    mov  %edi, %edi                     /* zero-extend the two arguments */
    mov  %esi, %esi
    call kmain
5:  cli
    hlt
    jmp  5b

/* void gdt_flush(const struct table_ptr *p): load the GDT, reload segments */
.global gdt_flush
gdt_flush:
    lgdt (%rdi)
    mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %ss
    xor  %eax, %eax
    mov  %ax, %fs
    mov  %ax, %gs
    pop  %rax                           /* return address */
    push $0x08                          /* far return reloads CS */
    push %rax
    lretq

/* void tss_flush(void) */
.global tss_flush
tss_flush:
    mov  $0x28, %ax
    ltr  %ax
    ret

/* void idt_flush(const struct table_ptr *p) */
.global idt_flush
idt_flush:
    lidt (%rdi)
    ret

/* void enter_user(u64 entry, u64 user_rsp, u64 arg1, u64 arg2)
 * Drop to ring 3 and never return. The next interrupt or system call starts
 * at the top of the task's kernel stack (TSS.rsp0). */
.global enter_user
enter_user:
    mov  %rdx, %r8
    mov  %rcx, %r9
    push $0x23                          /* ss */
    push %rsi                           /* rsp */
    push $0x202                         /* rflags: interrupts enabled */
    push $0x1B                          /* cs */
    push %rdi                           /* rip */
    mov  %r8, %rdi                      /* arguments of the program's _start */
    mov  %r9, %rsi
    xor  %eax, %eax
    xor  %ebx, %ebx
    xor  %ecx, %ecx
    xor  %edx, %edx
    xor  %r8d, %r8d
    xor  %r9d, %r9d
    xor  %r10d, %r10d
    xor  %r11d, %r11d
    xor  %r12d, %r12d
    xor  %r13d, %r13d
    xor  %r14d, %r14d
    xor  %r15d, %r15d
    xor  %ebp, %ebp
    iretq

/* void switch_context(u64 *old_rsp, u64 new_rsp)
 * Saves the callee-saved state of the current task on its stack and resumes
 * another task (see task_create() for the initial frame layout). */
.global switch_context
switch_context:
    pushfq
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov  %rsp, (%rdi)
    mov  %rsi, %rsp
    pop  %r15
    pop  %r14
    pop  %r13
    pop  %r12
    pop  %rbx
    pop  %rbp
    popfq
    ret

.section .note.GNU-stack, "", @progbits
