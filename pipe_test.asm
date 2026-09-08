; pipe_test.asm
; Демонстрация pipe() + fork(): родитель пишет в трубу, ребёнок читает.
; SYS_WRITE=0, SYS_READ=1, SYS_EXIT=2, SYS_CLOSE=7, SYS_FORK=12, SYS_PIPE=18
;
; Сборка:
; nasm -f elf64 pipe_test.asm -o pipe_test.o
; ld -m elf_x86_64 -o pipe_test.elf pipe_test.o

global _start

section .text

_start:
    lea rdi, [rel fds]      ; int fds[2]
    mov rax, 18             ; SYS_PIPE
    syscall

    mov rax, 12             ; SYS_FORK
    syscall

    cmp rax, 0
    je .child

.parent:
    ; закрываем свой конец на чтение - он нам не нужен
    mov edi, [rel fds]
    mov rax, 7              ; SYS_CLOSE
    syscall

    ; пишем сообщение в конец на запись
    mov edi, [rel fds+4]
    lea rsi, [rel msg]
    mov rdx, msg_len
    mov rax, 0              ; SYS_WRITE
    syscall

    ; закрываем конец на запись - иначе ребёнок никогда не увидит EOF
    mov edi, [rel fds+4]
    mov rax, 7
    syscall

    mov rax, 2              ; SYS_EXIT
    mov rdi, 0
    syscall

.child:
    ; закрываем свой конец на запись - он нам не нужен
    mov edi, [rel fds+4]
    mov rax, 7
    syscall

    ; читаем из конца на чтение (заблокируемся, пока родитель не напишет)
    mov edi, [rel fds]
    lea rsi, [rel buf]
    mov rdx, buf_size
    mov rax, 1              ; SYS_READ
    syscall

    ; печатаем ровно то, что прочитали (rax = число байт)
    mov rdx, rax
    mov rdi, 1              ; stdout
    lea rsi, [rel buf]
    mov rax, 0              ; SYS_WRITE
    syscall

    mov rax, 2              ; SYS_EXIT
    mov rdi, 0
    syscall

section .data

msg: db 'Hello through the pipe!', 10
msg_len: equ $ - msg

section .bss

fds: resd 2
buf_size: equ 128
buf: resb buf_size
