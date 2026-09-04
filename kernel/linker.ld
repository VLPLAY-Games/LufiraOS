ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .text : {
        *(.text.prologue)
        *(.text)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        *(.bss*)
        *(COMMON)
    }

    __kernel_end = .;
}