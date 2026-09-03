; hello_simple.asm
; Напрямую вызывает sys_write (номер 0) и sys_exit (номер 2)
; Сборка: nasm -f elf64 hello_simple.asm -o hello_simple.o
;          ld -m elf_x86_64 -o hello.elf hello_simple.o

global _start
section .text
_start:
    ; sys_write(1, msg, len)
    mov rax, 0          ; sys_write
    mov rdi, 1          ; stdout
    lea rsi, [rel msg]
    mov rdx, len
    syscall

    ; sys_exit(0)
    mov rax, 2          ; sys_exit
    mov rdi, 0          ; exit code 0
    syscall

section .data
msg: db 'Hello!', 10
len: equ $ - msg