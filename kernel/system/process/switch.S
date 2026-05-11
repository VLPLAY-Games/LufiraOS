.section .text
.global context_switch
.type context_switch, @function

context_switch:
    # Сохраняем регистры общего назначения
    movq %rax, 0(%rdi)
    movq %rbx, 8(%rdi)
    movq %rcx, 16(%rdi)
    movq %rdx, 24(%rdi)
    
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
    
    # Восстанавливаем регистры общего назначения
    movq 0(%rsi), %rax
    movq 8(%rsi), %rbx
    movq 16(%rsi), %rcx
    movq 24(%rsi), %rdx
    
    # Восстанавливаем RIP и прыгаем
    movq 128(%rsi), %rax
    jmp *%rax

.size context_switch, .-context_switch