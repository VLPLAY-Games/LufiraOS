.section .text

.section .bss
.global user_rsp_save
user_rsp_save: .quad 0
.section .text

.extern current_kernel_rsp

.global syscall_entry
.type syscall_entry, @function

syscall_entry:
    # Сохраняем user RSP
    mov %rsp, %rbx
    mov %rbx, user_rsp_save(%rip)

    # Переключаемся на kernel stack
    mov current_kernel_rsp(%rip), %rsp

    # Сохраняем user context
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

    subq $8, %rsp

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

    # Восстанавливаем user RSP
    mov user_rsp_save(%rip), %rsp

    # Формируем стек для iretq
    pushq $0x2B               # user SS (данные)
    pushq user_rsp_save(%rip) # user RSP
    pushq %r11                # user RFLAGS
    pushq $0x33               # user CS
    pushq %rcx                # user RIP
    iretq

.size syscall_entry, .-syscall_entry