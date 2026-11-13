/* Interrupt entry stubs (64-bit): vectors 0-31 are CPU exceptions, 32-47 are
 * IRQ 0-15, 128 is the system call. In long mode the CPU always pushes
 * SS, RSP, RFLAGS, CS and RIP; the stubs add a uniform frame (struct regs). */
.altmacro
.code64

.macro ISR_NOERR n
isr_stub_\n:
    push $0                             /* fake error code */
    push $\n
    jmp  isr_common
.endm

.macro ISR_ERR n
isr_stub_\n:
    push $\n                            /* CPU already pushed an error code */
    jmp  isr_common
.endm

.macro TABLE_ENTRY n
    .quad isr_stub_\n
.endm

.section .text
.set i, 0
.rept 48
    .if (i == 8) || ((i >= 10) && (i <= 14)) || (i == 17) || (i == 21) || (i == 29) || (i == 30)
        ISR_ERR %i
    .else
        ISR_NOERR %i
    .endif
    .set i, i + 1
.endr

/* System call gate (int $0x80). */
.global isr_stub_syscall
isr_stub_syscall:
    push $0
    push $128
    jmp  isr_common

isr_common:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    cld
    mov  %rsp, %rdi                     /* struct regs * */
    mov  %rsp, %rbp                     /* rbp survives the call */
    and  $-16, %rsp                     /* the C ABI wants an aligned stack */
    call interrupt_dispatch
    mov  %rbp, %rsp
    pop  %r15
    pop  %r14
    pop  %r13
    pop  %r12
    pop  %r11
    pop  %r10
    pop  %r9
    pop  %r8
    pop  %rbp
    pop  %rdi
    pop  %rsi
    pop  %rdx
    pop  %rcx
    pop  %rbx
    pop  %rax
    add  $16, %rsp                      /* vector + error code */
    iretq

.section .rodata
.global isr_stub_table
.align 8
isr_stub_table:
.set i, 0
.rept 48
    TABLE_ENTRY %i
    .set i, i + 1
.endr

.section .note.GNU-stack, "", @progbits
