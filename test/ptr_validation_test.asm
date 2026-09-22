; ptr_validation_test.asm
; Проверка валидации user-указателей в syscall'ах: заведомо плохие адреса
; (адрес кучи ядра и низкий немаппленный-для-user адрес) должны давать
; -EFAULT (-14), а не падение системы и не тихий успех. В конце —
; позитивный контроль (валидный буфер должен по-прежнему срабатывать).
;
; SYS_WRITE=0, SYS_READ=1, SYS_EXIT=2, SYS_OPEN=6, SYS_FORK=12, SYS_WAIT=13,
; SYS_PIPE=18. EFAULT=14.
;
; Сборка:
; nasm -f elf64 ptr_validation_test.asm -o ptr_validation_test.o
; ld -m elf_x86_64 -o ptr_validation_test.elf ptr_validation_test.o

global _start

%macro PRINT 2
    mov rax, 0
    mov rdi, 1
    lea rsi, [rel %1]
    mov rdx, %2
    syscall
%endmacro

KERNEL_ADDR equ 0xFFFF900000000000   ; KERNEL_HEAP_START — присутствует, но не USER
LOW_UNMAPPED equ 0x1000              ; в сыром identity-map ядра, но не USER

section .text

_start:
    ; --- 1: SYS_WRITE в адрес кучи ядра ---
    mov rax, 0
    mov rdi, 1
    mov rsi, KERNEL_ADDR
    mov rdx, 16
    syscall
    cmp rax, -14
    jne .t1_fail
    PRINT t1_ok, t1_ok_len
    jmp .t2
.t1_fail:
    PRINT t1_fail, t1_fail_len

.t2:
    ; --- 2: SYS_WRITE в низкий немаппленный адрес ---
    mov rax, 0
    mov rdi, 1
    mov rsi, LOW_UNMAPPED
    mov rdx, 16
    syscall
    cmp rax, -14
    jne .t2_fail
    PRINT t2_ok, t2_ok_len
    jmp .t3
.t2_fail:
    PRINT t2_fail, t2_fail_len

.t3:
    ; --- 3: SYS_READ (need_write=1) в адрес кучи ядра ---
    mov rax, 1
    xor rdi, rdi
    mov rsi, KERNEL_ADDR
    mov rdx, 16
    syscall
    cmp rax, -14
    jne .t3_fail
    PRINT t3_ok, t3_ok_len
    jmp .t4
.t3_fail:
    PRINT t3_fail, t3_fail_len

.t4:
    ; --- 4: SYS_OPEN с filename в адресе кучи ядра ---
    mov rax, 6
    mov rdi, KERNEL_ADDR
    xor rsi, rsi
    xor rdx, rdx
    syscall
    cmp rax, -14
    jne .t4_fail
    PRINT t4_ok, t4_ok_len
    jmp .t5
.t4_fail:
    PRINT t4_fail, t4_fail_len

.t5:
    ; --- 5: SYS_PIPE с fds_ptr в адресе кучи ядра ---
    mov rax, 18
    mov rdi, KERNEL_ADDR
    syscall
    cmp rax, -14
    jne .t5_fail
    PRINT t5_ok, t5_ok_len
    jmp .t6
.t5_fail:
    PRINT t5_fail, t5_fail_len

.t6:
    ; --- 6: SYS_WAIT с плохим status_ptr (после fork) ---
    mov rax, 12                ; SYS_FORK
    syscall
    cmp rax, 0
    je .t6_child

.t6_parent:
    xor rdi, rdi
    mov rsi, KERNEL_ADDR
    xor rdx, rdx
    mov rax, 13                 ; SYS_WAIT
    syscall
    cmp rax, -14
    jne .t6_fail
    PRINT t6_ok, t6_ok_len
    ; ребёнок ещё не отреапан (process_wait() выше не вызывался) — реапим
    ; нормальным wait(status_ptr=0), чтобы не плодить зомби
    xor rdi, rdi
    xor rsi, rsi
    xor rdx, rdx
    mov rax, 13
    syscall
    jmp .t7
.t6_fail:
    PRINT t6_fail, t6_fail_len
    xor rdi, rdi
    xor rsi, rsi
    xor rdx, rdx
    mov rax, 13
    syscall
    jmp .t7

.t6_child:
    mov rax, 2                  ; SYS_EXIT
    xor rdi, rdi
    syscall

.t7:
    ; --- позитивный контроль: валидный стековый/.data буфер должен работать ---
    lea rsi, [rel positive_msg]
    mov rdi, 1
    mov rdx, positive_msg_len
    mov rax, 0
    syscall
    cmp rax, positive_msg_len
    jne .t7_fail
    PRINT t7_ok, t7_ok_len
    jmp .done
.t7_fail:
    PRINT t7_fail, t7_fail_len

.done:
    PRINT all_done, all_done_len
    mov rax, 2
    xor rdi, rdi
    syscall

section .data

positive_msg:     db 'valid-buffer-write', 10
positive_msg_len: equ $ - positive_msg

t1_ok:      db 'write-kernel-addr EFAULT OK', 10
t1_ok_len:  equ $ - t1_ok
t1_fail:    db 'write-kernel-addr EFAULT FAIL', 10
t1_fail_len: equ $ - t1_fail

t2_ok:      db 'write-low-unmapped EFAULT OK', 10
t2_ok_len:  equ $ - t2_ok
t2_fail:    db 'write-low-unmapped EFAULT FAIL', 10
t2_fail_len: equ $ - t2_fail

t3_ok:      db 'read-kernel-addr EFAULT OK', 10
t3_ok_len:  equ $ - t3_ok
t3_fail:    db 'read-kernel-addr EFAULT FAIL', 10
t3_fail_len: equ $ - t3_fail

t4_ok:      db 'open-kernel-addr EFAULT OK', 10
t4_ok_len:  equ $ - t4_ok
t4_fail:    db 'open-kernel-addr EFAULT FAIL', 10
t4_fail_len: equ $ - t4_fail

t5_ok:      db 'pipe-kernel-addr EFAULT OK', 10
t5_ok_len:  equ $ - t5_ok
t5_fail:    db 'pipe-kernel-addr EFAULT FAIL', 10
t5_fail_len: equ $ - t5_fail

t6_ok:      db 'wait-kernel-addr EFAULT OK', 10
t6_ok_len:  equ $ - t6_ok
t6_fail:    db 'wait-kernel-addr EFAULT FAIL', 10
t6_fail_len: equ $ - t6_fail

t7_ok:      db 'positive-control OK', 10
t7_ok_len:  equ $ - t7_ok
t7_fail:    db 'positive-control FAIL', 10
t7_fail_len: equ $ - t7_fail

all_done:     db 'all tests done', 10
all_done_len: equ $ - all_done
