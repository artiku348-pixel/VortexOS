/* VortexOS entry point (Multiboot 1, 32-bit protected mode). */
.set MB_MAGIC, 0x1BADB002
.set MB_FLAGS, 0x3                      /* page-align modules + memory info */
.set MB_CHECK, -(MB_MAGIC + MB_FLAGS)

.section .multiboot, "a"
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECK

.section .bss
.align 16
stack_bottom:
.skip 16384
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    cli
    mov  $stack_top, %esp
    xor  %ebp, %ebp
    push %ebx                           /* multiboot info pointer */
    push %eax                           /* multiboot magic */
    call kmain
1:  cli
    hlt
    jmp  1b

/* void gdt_flush(const struct table_ptr *p) */
.global gdt_flush
gdt_flush:
    mov  4(%esp), %eax
    lgdt (%eax)
    mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %fs
    mov  %ax, %gs
    mov  %ax, %ss
    ljmp $0x08, $1f
1:  ret

/* void idt_flush(const struct table_ptr *p) */
.global idt_flush
idt_flush:
    mov  4(%esp), %eax
    lidt (%eax)
    ret

/* void switch_context(u32 *old_esp, u32 new_esp)
 * Saves the callee-saved state of the current task on its stack and resumes
 * another task (see task_create() for the initial frame layout). */
.global switch_context
switch_context:
    mov  4(%esp), %eax
    mov  8(%esp), %edx
    pushf
    push %ebp
    push %ebx
    push %esi
    push %edi
    mov  %esp, (%eax)
    mov  %edx, %esp
    pop  %edi
    pop  %esi
    pop  %ebx
    pop  %ebp
    popf
    ret

.section .note.GNU-stack, "", @progbits
