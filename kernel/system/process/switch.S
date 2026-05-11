.section .text
.global context_switch
.type context_switch, @function

context_switch:
    # Сохраняем регистры
    movq %rax, 0(%rdi)
    movq %rbx, 8(%rdi)
    movq %rcx, 16(%rdi)
    movq %rdx, 24(%rdi)
    movq %rsi, 32(%rdi)
    movq %rdi, 40(%rdi)
    movq %rbp, 48(%rdi)
    
    # Сохраняем RIP
    movq (%rsp), %rax
    movq %rax, 128(%rdi)
    
    # Сохраняем RSP
    leaq 8(%rsp), %rax
    movq %rax, 120(%rdi)
    
    # Сохраняем RFLAGS
    pushfq
    popq %rax
    movq %rax, 136(%rdi)
    
    # Сохраняем CR3
    movq %cr3, %rax
    movq %rax, 144(%rdi)
    
    # ========================================
    
    # Восстанавливаем CR3
    movq 144(%rsi), %rax
    movq %rax, %cr3
    
    # Восстанавливаем RFLAGS
    movq 136(%rsi), %rax
    pushq %rax
    popfq
    
    # Восстанавливаем RSP
    movq 120(%rsi), %rsp
    
    # Сохраняем RIP в r10 (временный регистр)
    movq 128(%rsi), %r10
    
    # Восстанавливаем регистры (КРОМЕ RSI)
    movq 0(%rsi), %rax
    movq 8(%rsi), %rbx
    movq 16(%rsi), %rcx
    movq 24(%rsi), %rdx
    movq 48(%rsi), %rbp
    movq 40(%rsi), %rdi
    
    # Восстанавливаем RSI последним
    movq 32(%rsi), %rsi
    
    # Прыгаем на сохранённый RIP
    jmp *%r10

.size context_switch, .-context_switch