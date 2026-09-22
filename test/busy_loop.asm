; busy_loop.asm
; Тест вытесняющей многозадачности: программа, которая НИ РАЗУ не делает
; syscall (даже SYS_EXIT) — крутит бесконечный счётчик. До появления
; context_enter_ring3()/timer-preemption такая программа никогда не
; покидала ring0 и не могла быть вытеснена таймером, поэтому одна
; занимала процессор навсегда (см. Makefile-цель debug и план в
; documentation — сценарий проверки preemptive multitasking).
;
; Сборка:
; nasm -f elf64 busy_loop.asm -o busy_loop.o
; ld -m elf_x86_64 -o busy_loop.elf busy_loop.o

global _start

section .text

_start:
    xor rax, rax
.loop:
    inc rax
    jmp .loop
