ENTRY(_start)

SECTIONS
{
    . = 0x100000;

    .text : {
        *(.text.prologue)
        *(.text)
    }

    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata*)
    }

    . = ALIGN(0x1000);
    .data : {
        *(.data*)
    }

    .bss : {
        *(.bss*)
        *(COMMON)
    }

    __kernel_end = .;
}