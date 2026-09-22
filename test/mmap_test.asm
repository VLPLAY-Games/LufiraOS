; mmap_test.asm
; Проверка sys_mmap()/sys_munmap(): выделение, обнуление, чтение/запись на
; границах региона, повторный mmap без коллизии адресов, и то, что fork()
; физически дублирует mmap-регион (родитель и ребёнок видят раздельные
; копии одной и той же виртуальной памяти).
;
; SYS_WRITE=0, SYS_EXIT=2, SYS_MMAP=9, SYS_MUNMAP=10, SYS_FORK=12, SYS_WAIT=13
; PROT_READ=1, PROT_WRITE=2 -> 3
; MAP_PRIVATE=2, MAP_ANONYMOUS=0x20 -> 0x22
;
; Сборка:
; nasm -f elf64 mmap_test.asm -o mmap_test.o
; ld -m elf_x86_64 -o mmap_test.elf mmap_test.o

global _start

%macro PRINT 2
    mov rax, 0              ; SYS_WRITE
    mov rdi, 1              ; stdout
    lea rsi, [rel %1]
    mov rdx, %2
    syscall
%endmacro

section .text

_start:
    ; --- mmap: 2 страницы (8192 байт) ---
    mov rax, 9               ; SYS_MMAP
    xor rdi, rdi              ; addr (игнорируется)
    mov rsi, 8192              ; length
    mov rdx, 3                  ; PROT_READ|PROT_WRITE
    mov r10, 0x22                 ; MAP_PRIVATE|MAP_ANONYMOUS
    xor r8, r8                     ; fd (игнорируется)
    syscall

    cmp rax, -1
    je .mmap1_fail
    mov rbx, rax              ; rbx = база региона 1 (переживает syscall'ы)
    PRINT mmap1_ok, mmap1_ok_len
    jmp .zero_check
.mmap1_fail:
    PRINT mmap1_fail, mmap1_fail_len
    jmp .done

.zero_check:
    ; середина второй страницы должна быть уже обнулена
    mov rax, [rbx + 4096]
    test rax, rax
    jnz .zero_fail
    PRINT zero_ok, zero_ok_len
    jmp .readwrite
.zero_fail:
    PRINT zero_fail, zero_fail_len

.readwrite:
    ; пишем на начало региона и на последние 8 байт второй страницы —
    ; конец региона проверяет, что eager-выделение замаппило ВСЕ страницы,
    ; а не только первую
    mov r15, 0xDEADBEEFCAFEBABE
    mov [rbx], r15
    mov r14, 0x1122334455667788
    mov [rbx + 8184], r14

    mov rax, [rbx]
    cmp rax, r15
    jne .rw_fail
    mov rax, [rbx + 8184]
    cmp rax, r14
    jne .rw_fail
    PRINT rw_ok, rw_ok_len
    jmp .munmap_test
.rw_fail:
    PRINT rw_fail, rw_fail_len

.munmap_test:
    mov rax, 10               ; SYS_MUNMAP
    mov rdi, rbx
    mov rsi, 8192
    syscall

    test rax, rax
    jnz .munmap_fail
    PRINT munmap_ok, munmap_ok_len
    jmp .nocollide_test
.munmap_fail:
    PRINT munmap_fail, munmap_fail_len

.nocollide_test:
    mov rax, 9                ; SYS_MMAP
    xor rdi, rdi
    mov rsi, 4096
    mov rdx, 3
    mov r10, 0x22
    xor r8, r8
    syscall

    cmp rax, -1
    je .nocollide_fail
    cmp rax, rbx
    jle .nocollide_fail        ; bump-указатель только растёт — новый адрес должен быть выше
    PRINT nocollide_ok, nocollide_ok_len
    jmp .fork_test
.nocollide_fail:
    PRINT nocollide_fail, nocollide_fail_len

.fork_test:
    mov rax, 9                 ; SYS_MMAP — отдельный регион под fork-тест
    xor rdi, rdi
    mov rsi, 4096
    mov rdx, 3
    mov r10, 0x22
    xor r8, r8
    syscall

    cmp rax, -1
    je .done
    mov r13, rax               ; r13 = база fork-региона

    lea r12, [rel parent_pattern]
    mov r15, [r12]
    mov [r13], r15              ; пишем 'PARENT!!'

    mov rax, 12                  ; SYS_FORK
    syscall

    cmp rax, 0
    je .child

.parent:
    xor rdi, rdi                 ; wait(0, NULL, 0) — любой ребёнок
    xor rsi, rsi
    xor rdx, rdx
    mov rax, 13                   ; SYS_WAIT
    syscall

    mov rax, [r13]
    lea rbx, [rel parent_pattern]
    mov rcx, [rbx]
    cmp rax, rcx
    jne .fork_fail
    PRINT fork_ok, fork_ok_len
    jmp .done
.fork_fail:
    PRINT fork_fail, fork_fail_len
    jmp .done

.child:
    lea r12, [rel child_pattern]
    mov r15, [r12]
    mov [r13], r15                 ; ребёнок пишет 'CHILD!!!' в СВОЮ копию
    PRINT child_wrote, child_wrote_len
    mov rax, 2                      ; SYS_EXIT
    xor rdi, rdi
    syscall

.done:
    PRINT all_done, all_done_len
    mov rax, 2                       ; SYS_EXIT
    xor rdi, rdi
    syscall

section .data

parent_pattern: db 'PARENT!!'
child_pattern:  db 'CHILD!!!'

mmap1_ok:       db 'mmap1 OK', 10
mmap1_ok_len:   equ $ - mmap1_ok
mmap1_fail:     db 'mmap1 FAIL', 10
mmap1_fail_len: equ $ - mmap1_fail

zero_ok:        db 'zero OK', 10
zero_ok_len:    equ $ - zero_ok
zero_fail:      db 'zero FAIL', 10
zero_fail_len:  equ $ - zero_fail

rw_ok:          db 'readwrite OK', 10
rw_ok_len:      equ $ - rw_ok
rw_fail:        db 'readwrite FAIL', 10
rw_fail_len:    equ $ - rw_fail

munmap_ok:      db 'munmap OK', 10
munmap_ok_len:  equ $ - munmap_ok
munmap_fail:    db 'munmap FAIL', 10
munmap_fail_len: equ $ - munmap_fail

nocollide_ok:   db 'nocollide OK', 10
nocollide_ok_len: equ $ - nocollide_ok
nocollide_fail: db 'nocollide FAIL', 10
nocollide_fail_len: equ $ - nocollide_fail

child_wrote:    db 'fork-child-wrote OK', 10
child_wrote_len: equ $ - child_wrote

fork_ok:        db 'fork-parent-intact OK', 10
fork_ok_len:    equ $ - fork_ok
fork_fail:      db 'fork-parent-intact FAIL', 10
fork_fail_len:  equ $ - fork_fail

all_done:       db 'all tests done', 10
all_done_len:   equ $ - all_done
