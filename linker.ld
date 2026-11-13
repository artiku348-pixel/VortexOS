ENTRY(_start)

SECTIONS
{
    . = 1M;                     /* the bootloader loads us at 1 MiB */
    _kernel_start = .;

    .text ALIGN(4K) : {
        KEEP(*(.multiboot))     /* header must be in the first 8 KiB */
        *(.text .text.*)
    }
    .rodata ALIGN(4K) : { *(.rodata .rodata.*) }
    .data   ALIGN(4K) : { *(.data .data.*) }
    .bss    ALIGN(4K) : { *(COMMON) *(.bss .bss.*) }

    _kernel_end = .;

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
