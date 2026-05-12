.section .text

.global syscall_entry
.type syscall_entry, @function

#
# x86_64 syscall entry
#
# Вход от инструкции syscall:
#
#   RAX = syscall number
#   RDI = arg1
#   RSI = arg2
#   RDX = arg3
#   R10 = arg4
#   R8  = arg5
#   R9  = arg6
#
# RCX = user RIP
# R11 = user RFLAGS
#
# SysV ABI для syscall_handler:
#
#   RDI = syscall number
#   RSI = arg1
#   RDX = arg2
#   RCX = arg3
#   R8  = arg4
#   R9  = arg5
#
# Возврат:
#   RAX = результат syscall
#

syscall_entry:
    #
    # Сохраняем user context
    #

    pushq %rcx                # user RIP
    pushq %r11                # user RFLAGS

    #
    # Сохраняем регистры,
    # которые хотим восстановить после syscall
    #

    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    #
    # Сохраняем syscall arguments
    #

    pushq %rax                # syscall number
    pushq %rdi                # arg1
    pushq %rsi                # arg2
    pushq %rdx                # arg3
    pushq %r10                # arg4
    pushq %r8                 # arg5
    pushq %r9                 # arg6

    #
    # После 15 push стек misaligned на 8 байт.
    # Перед call нужен 16-byte alignment.
    #

    subq $8, %rsp

    #
    # Готовим аргументы для syscall_handler
    #
    # syscall_handler(
    #     syscall_num,
    #     arg1,
    #     arg2,
    #     arg3,
    #     arg4,
    #     arg5
    # )
    #

    movq 56(%rsp), %rdi       # syscall number
    movq 48(%rsp), %rsi       # arg1
    movq 40(%rsp), %rdx       # arg2
    movq 32(%rsp), %rcx       # arg3
    movq 24(%rsp), %r8        # arg4
    movq 16(%rsp), %r9        # arg5

    #
    # Вызываем C handler
    #

    call syscall_handler

    #
    # Убираем alignment padding
    #

    addq $8, %rsp

    #
    # Восстанавливаем сохранённые syscall registers
    #

    popq %r9
    popq %r8
    popq %r10
    popq %rdx
    popq %rsi
    popq %rdi

    #
    # НЕ восстанавливаем старый RAX,
    # потому что syscall return value должен остаться в RAX
    #

    addq $8, %rsp             # пропускаем сохранённый syscall number

    #
    # Восстанавливаем callee-saved registers
    #

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx

    #
    # Восстанавливаем user state для sysretq
    #

    popq %r11                 # user RFLAGS
    popq %rcx                 # user RIP

    #
    # Возврат в usermode
    #

    sysretq

.size syscall_entry, .-syscall_entry