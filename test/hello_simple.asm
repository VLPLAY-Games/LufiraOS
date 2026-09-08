; hello_simple.asm
; sys_write  = 0
; sys_exit   = 2
; sys_sleep  = 16
;
; Сборка:
; nasm -f elf64 hello_simple.asm -o hello_simple.o
; ld -m elf_x86_64 -o hello.elf hello_simple.o

global _start

section .text

_start:

.loop:
    ; sys_write(1, msg, len)
    mov rax, 0              ; SYS_WRITE
    mov rdi, 1              ; stdout
    lea rsi, [rel msg]
    mov rdx, len
    syscall

    ; sys_sleep(1000)
    mov rax, 16             ; SYS_SLEEP
    mov rdi, 1000           ; 1000 ms
    syscall

    jmp .loop

    ; До сюда пока не дойдём
    mov rax, 2              ; SYS_EXIT
    mov rdi, 0
    syscall

section .data

msg: db 'Hello!', 10
len: equ $ - msg