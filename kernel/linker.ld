ENTRY(_start)

SECTIONS {
    . = 0x100000;
    
    .text : {
        *(.text.prologue) /* Сначала точка входа */
        *(.text)
    }
    
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
}
