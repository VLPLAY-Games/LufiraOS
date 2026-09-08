.section .text

.section .bss
.global user_rsp_save
user_rsp_save: .quad 0
.section .text

.extern current_kernel_rsp

.global syscall_entry
.type syscall_entry, @function

syscall_entry:
    # user_rsp_save здесь используется ТОЛЬКО как сиюминутный мост между
    # соседними инструкциями (прерывания уже маскированы FMASK на входе
    # в syscall, вклиниться некому) — переносим user RSP на СОБСТВЕННЫЙ
    # ядерный стек этого процесса ниже. Раньше эта переменная была общей
    # на всё ядро и служила единственным хранилищем user RSP до самого
    # эпилога — но syscall может приостановиться на произвольное время
    # (schedule() -> context_switch(), например у SYS_SLEEP/SYS_WAIT), и
    # если за это время СВОЙ syscall сделает другой процесс, он перезапишет
    # эту общую переменную, и проснувшийся процесс в эпилоге восстановит
    # ЧУЖОЙ user-mode стек. Заодно НЕ трогаем %rbx для этого — раньше
    # "mov %rsp, %rbx" затирал оригинальный user %rbx ДО того, как он
    # сохранялся чуть ниже как callee-saved регистр, и терялся навсегда.
    mov %rsp, user_rsp_save(%rip)
    mov current_kernel_rsp(%rip), %rsp

    # Сохраняем user context
    pushq user_rsp_save(%rip) # user RSP (теперь на своём стеке)
    pushq $0                  # паддинг: восстанавливает 16-байтное
                               # выравнивание перед call ниже (см. итоговый
                               # комментарий у "mov %rsp, %r11")
    pushq %rcx                # user RIP
    pushq %r11                # user RFLAGS

    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    pushq %rax                # syscall number
    pushq %rdi                # arg1
    pushq %rsi                # arg2
    pushq %rdx                # arg3
    pushq %r10                # arg4
    pushq %r8                 # arg5
    pushq %r9                 # arg6

    # %rsp сейчас указывает точно на начало сохранённого кадра регистров
    # (см. process.c: syscall_frame_t). Передаём этот указатель 7-м
    # (стековым, по System V ABI) аргументом в syscall_handler — нужен
    # SYS_FORK (чтобы скопировать точное состояние родителя в дочерний
    # процесс, включая user RSP из хвоста этого кадра) и одновременно
    # служит той же выравнивающей рамкой, что раньше давал "subq $8, %rsp"
    # (в сумме с паддингом выше — 18 push'ей = 144 байта, кратно 16).
    mov %rsp, %r11
    pushq %r11

    movq 56(%rsp), %rdi       # syscall number
    movq 48(%rsp), %rsi       # arg1
    movq 40(%rsp), %rdx       # arg2
    movq 32(%rsp), %rcx       # arg3
    movq 24(%rsp), %r8        # arg4
    movq 16(%rsp), %r9        # arg5

    call syscall_handler

    addq $8, %rsp

    popq %r9
    popq %r8
    popq %r10
    popq %rdx
    popq %rsi
    popq %rdi

    addq $8, %rsp             # пропускаем syscall number

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx

    popq %r11                 # user RFLAGS
    popq %rcx                 # user RIP
    addq $8, %rsp             # пропускаем паддинг

    # Восстанавливаем user RSP СО СВОЕГО СТЕКА (не из общей переменной —
    # см. комментарий в начале функции) через тот же сиюминутный мост.
    popq user_rsp_save(%rip)

    # Формируем стек для iretq
    pushq $0x2B               # user SS (данные)
    pushq user_rsp_save(%rip) # user RSP
    pushq %r11                # user RFLAGS
    pushq $0x33               # user CS
    pushq %rcx                # user RIP
    iretq

.size syscall_entry, .-syscall_entry

.section .note.GNU-stack,"",@progbits
