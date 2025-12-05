; kernel_entry.asm - точка входа в 32-битное ядро LufiraOS
bits 32
global _start
extern kernel_main    ; Объявляем внешнюю C-функцию

section .text
_start:
    ; Устанавливаем сегментные регистры для данных
    mov ax, 0x10      ; Селектор сегмента данных из GDT
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Устанавливаем указатель стека
    mov esp, stack_top
    
    ; Вызываем главную функцию ядра на C
    call kernel_main
    
    ; Если kernel_main вернется (чего не должно быть)
    cli
.hang:
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 16384        ; 16KB для стека
stack_top: