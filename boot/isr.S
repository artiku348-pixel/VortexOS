/* Interrupt entry stubs: vectors 0-31 are CPU exceptions, 32-47 are IRQ 0-15. */
.altmacro

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
    .long isr_stub_\n
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

isr_common:
    pusha
    push %ds
    push %es
    mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    cld
    push %esp                           /* struct regs * */
    call interrupt_dispatch
    add  $4, %esp
    pop  %es
    pop  %ds
    popa
    add  $8, %esp                       /* vector + error code */
    iret

.section .rodata
.global isr_stub_table
.align 4
isr_stub_table:
.set i, 0
.rept 48
    TABLE_ENTRY %i
    .set i, i + 1
.endr

.section .note.GNU-stack, "", @progbits
