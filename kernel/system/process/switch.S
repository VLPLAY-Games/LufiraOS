.section .text
.global context_switch
.type context_switch, @function

# void context_switch(process_context_t *old_context, process_context_t *new_context)
# RDI = указатель на old_context
# RSI = указатель на new_context
context_switch:
    # Сохраняем регистры в old_context
    movq %rax, 0(%rdi)
    movq %rbx, 8(%rdi)
    movq %rcx, 16(%rdi)
    movq %rdx, 24(%rdi)
    movq %rsi, 32(%rdi)
    movq %rdi, 40(%rdi)
    movq %rbp, 48(%rdi)
    
    movq %r8,  56(%rdi)
    movq %r9,  64(%rdi)
    movq %r10, 72(%rdi)
    movq %r11, 80(%rdi)
    movq %r12, 88(%rdi)
    movq %r13, 96(%rdi)
    movq %r14, 104(%rdi)
    movq %r15, 112(%rdi)
    
    # Сохраняем RSP (указатель на вершину стека ДО вызова)
    # После call на стеке лежит адрес возврата
    leaq 8(%rsp), %rax
    movq %rax, 120(%rdi)   # rsp
    
    # Сохраняем RIP (адрес возврата)
    movq (%rsp), %rax
    movq %rax, 128(%rdi)   # rip
    
    # Сохраняем RFLAGS
    pushfq
    popq %rax
    movq %rax, 136(%rdi)   # rflags
    
    # Сохраняем CR3
    movq %cr3, %rax
    movq %rax, 144(%rdi)   # cr3
    
    # ========================================
    # Восстанавливаем контекст из new_context
    # ========================================
    
    # Восстанавливаем CR3
    movq 144(%rsi), %rax
    movq %cr3, %rax
    cmpq %rax, 144(%rsi)
    je 1f
    movq 144(%rsi), %rax
    movq %rax, %cr3
1:
    
    # Восстанавливаем RFLAGS
    movq 136(%rsi), %rax
    pushq %rax
    popfq
    
    # Восстанавливаем RSP (переключаем стек!)
    movq 120(%rsi), %rsp
    
    # Восстанавливаем RIP в RAX
    movq 128(%rsi), %rax
    
    # Восстанавливаем остальные регистры
    movq 0(%rsi), %r8    # временно используем r8 для rax
    movq 8(%rsi), %rbx
    movq 16(%rsi), %rcx
    movq 24(%rsi), %rdx
    movq 48(%rsi), %rbp
    
    movq 56(%rsi), %r9
    movq 64(%rsi), %r10
    movq 72(%rsi), %r11
    movq 80(%rsi), %r12
    movq 88(%rsi), %r13
    movq 96(%rsi), %r14
    movq 104(%rsi), %r15
    
    # Восстанавливаем RDI и RSI
    movq 40(%rsi), %rdi
    
    # RSI нужно восстановить последним
    movq 32(%rsi), %rsi
    
    # Восстанавливаем RAX
    movq %r8, %rax
    
    # "Возвращаемся" в новый процесс
    # RIP уже в RAX
    jmp *%rax

.size context_switch, .-context_switch