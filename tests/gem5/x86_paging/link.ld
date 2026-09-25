ENTRY(_start)
SECTIONS {
    . = 0x100000;
    .text : { *(.text.boot) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) BYTE(0) }
    /DISCARD/ : { *(.eh_frame*) *(.comment*) }
}
