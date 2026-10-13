/* legacy_main.ld — тот же приём, что kernel/linker.ld: плоский бинарник,
 * точка входа = самый первый байт (.text.prologue), грузится по
 * фиксированному физическому адресу (должен совпадать с
 * LEGACY_MAIN_LOAD_ADDR в layout.inc). */
ENTRY(legacy_main)

SECTIONS
{
    . = 0x90000;

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

    __legacy_main_end = .;
}
