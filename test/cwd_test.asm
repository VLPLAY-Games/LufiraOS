; cwd_test.asm
; Проверка sys_chdir()/sys_getcwd(): успешный переход, getcwd после него,
; ENOENT на несуществующий путь, ENOTDIR на переход в обычный файл, ERANGE
; на слишком маленький буфер. Специально НЕ делает exec() и завершается
; нормально через SYS_EXIT — после выхода cwd шелла (родителя) не должен
; измениться (проверяется отдельно командой pwd в шелле, руками/скриптом).
;
; SYS_WRITE=0, SYS_EXIT=2, SYS_GETCWD=14, SYS_CHDIR=15.
; ENOENT=2, ENOTDIR=20, ERANGE=34.
;
; Сборка:
; nasm -f elf64 cwd_test.asm -o cwd_test.o
; ld -m elf_x86_64 -o cwd_test.elf cwd_test.o

global _start

%macro PRINT 2
    mov rax, 0
    mov rdi, 1
    lea rsi, [rel %1]
    mov rdx, %2
    syscall
%endmacro

section .text

_start:
    ; --- 1: chdir("/system") -> 0 ---
    mov rax, 15                 ; SYS_CHDIR
    lea rdi, [rel path_system]
    syscall
    test rax, rax
    jnz .t1_fail
    PRINT t1_ok, t1_ok_len
    jmp .t2
.t1_fail:
    PRINT t1_fail, t1_fail_len

.t2:
    ; --- 2: getcwd() -> "/system", длина 7 ---
    mov rax, 14                  ; SYS_GETCWD
    lea rdi, [rel buf]
    mov rsi, 64
    syscall
    cmp rax, 7
    jne .t2_fail

    mov rsi, [rel buf]           ; первые 8 байт буфера как qword
    lea rdi, [rel path_system]
    mov rcx, [rdi]
    cmp rsi, rcx
    jne .t2_fail
    PRINT t2_ok, t2_ok_len
    jmp .t3
.t2_fail:
    PRINT t2_fail, t2_fail_len

.t3:
    ; --- 3: chdir на несуществующий путь -> -ENOENT ---
    mov rax, 15
    lea rdi, [rel path_missing]
    syscall
    cmp rax, -2
    jne .t3_fail
    PRINT t3_ok, t3_ok_len
    jmp .t4
.t3_fail:
    PRINT t3_fail, t3_fail_len

.t4:
    ; --- 4: chdir на обычный файл (не директорию) -> -ENOTDIR ---
    mov rax, 15
    lea rdi, [rel path_readme]
    syscall
    cmp rax, -20
    jne .t4_fail
    PRINT t4_ok, t4_ok_len
    jmp .t5
.t4_fail:
    PRINT t4_fail, t4_fail_len

.t5:
    ; --- 5: getcwd() со слишком маленьким буфером -> -ERANGE ---
    ; cwd сейчас всё ещё "/system" (шаги 3 и 4 не должны были его изменить)
    mov rax, 14
    lea rdi, [rel buf]
    mov rsi, 3
    syscall
    cmp rax, -34
    jne .t5_fail
    PRINT t5_ok, t5_ok_len
    jmp .done
.t5_fail:
    PRINT t5_fail, t5_fail_len

.done:
    PRINT all_done, all_done_len
    mov rax, 2                    ; SYS_EXIT — без exec(), чтобы не спутать
    xor rdi, rdi                   ; с проверкой "cwd шелла не поменялся"
    syscall

section .data

path_system:  db '/system', 0
path_missing: db '/does_not_exist', 0
path_readme:  db '/readme.txt', 0

t1_ok:      db 'chdir-system OK', 10
t1_ok_len:  equ $ - t1_ok
t1_fail:    db 'chdir-system FAIL', 10
t1_fail_len: equ $ - t1_fail

t2_ok:      db 'getcwd-matches OK', 10
t2_ok_len:  equ $ - t2_ok
t2_fail:    db 'getcwd-matches FAIL', 10
t2_fail_len: equ $ - t2_fail

t3_ok:      db 'chdir-enoent OK', 10
t3_ok_len:  equ $ - t3_ok
t3_fail:    db 'chdir-enoent FAIL', 10
t3_fail_len: equ $ - t3_fail

t4_ok:      db 'chdir-enotdir OK', 10
t4_ok_len:  equ $ - t4_ok
t4_fail:    db 'chdir-enotdir FAIL', 10
t4_fail_len: equ $ - t4_fail

t5_ok:      db 'getcwd-erange OK', 10
t5_ok_len:  equ $ - t5_ok
t5_fail:    db 'getcwd-erange FAIL', 10
t5_fail_len: equ $ - t5_fail

all_done:     db 'all tests done', 10
all_done_len: equ $ - all_done

section .bss
buf: resb 64
