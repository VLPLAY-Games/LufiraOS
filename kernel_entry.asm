; kernel_entry.asm - точка входа в 32-битное ядро
bits 32
global _start
extern kernel_main    ; Объявляем внешнюю C-функцию

section .text
_start:
    ; Устанавливаем указатель стека
    mov esp, stack_space
    
    ; Вызываем главную функцию ядра на C
    call kernel_main
    
    ; Если kernel_main вернется (чего не должно быть)
    cli
.hang:
    hlt
    jmp .hang

section .bss
resb 8192             ; 8KB для стека
stack_space: