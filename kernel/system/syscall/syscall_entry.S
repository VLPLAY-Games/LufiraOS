.section .text
.global syscall_entry
.type syscall_entry, @function

# Точка входа при выполнении инструкции syscall
# Регистры (согласно System V ABI):
# RAX = номер системного вызова
# RDI = arg1
# RSI = arg2
# RDX = arg3
# R10 = arg4 (вместо RCX, так как RCX сохраняет RIP)
# R8  = arg5
# R9  = arg6 (если нужно)

syscall_entry:
    # Сохраняем все регистры на стеке
    pushq %rcx    # Сохраняем user RIP (был в RCX)
    pushq %r11    # Сохраняем user RFLAGS (был в R11)
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11    # Дополнительно сохраняем (будем использовать для RFLAGS)
    pushq %r10    # arg4
    pushq %r9
    pushq %r8     # arg5
    pushq %rdi    # arg1
    pushq %rsi    # arg2
    pushq %rdx    # arg3
    pushq %rbp
    pushq %rbx
    pushq %rax    # syscall number
    
    # Вызываем C-обработчик
    # Передаём 6 аргументов + номер syscall
    movq %rax, %rdi           # 1-й аргумент: syscall_num
    popq %rax                 # Восстанавливаем RAX (был syscall_num до push)
    pushq %rax                # Снова сохраняем для правильного стека
    
    # Аргументы уже на стеке, передаём их через регистры
    # arg1 (RDI) уже на месте
    movq %rsi, %rsi           # 2-й аргумент: arg2
    movq %rdx, %rdx           # 3-й аргумент: arg3
    movq %r10, %rcx           # 4-й аргумент: arg4
    movq %r8, %r8             # 5-й аргумент: arg5
    movq %r9, %r9             # 6-й аргумент: arg6
    
    # Вызываем обработчик
    call syscall_handler
    
    # Восстанавливаем регистры
    popq %rax     # syscall_num
    popq %rbx
    popq %rbp
    popq %rdx     # arg3
    popq %rsi     # arg2
    popq %rdi     # arg1
    popq %r8      # arg5
    popq %r9
    popq %r10     # arg4
    popq %r11     # extra R11
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    popq %r11     # user RFLAGS
    popq %rcx     # user RIP
    
    # Возвращаемся в пользовательский код
    # RAX уже содержит возвращаемое значение
    sysretq

.size syscall_entry, .-syscall_entry