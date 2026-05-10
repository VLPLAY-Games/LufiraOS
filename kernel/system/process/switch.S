.section .text
.global context_switch
.type context_switch, @function

# void context_switch(process_context_t *old_context, process_context_t *new_context)
# RDI = указатель на old_context
# RSI = указатель на new_context
context_switch:
    # Сохраняем ВСЕ регистры в old_context (на который указывает RDI)
    
    # Сначала сохраняем регистры общего назначения
    movq %rax, 0(%rdi)
    movq %rbx, 8(%rdi)
    movq %rcx, 16(%rdi)
    movq %rdx, 24(%rdi)
    # RSI сохраняем, но помним что в нём пока old_context
    movq %rsi, 32(%rdi)
    # RDI нужно сохранить ДО того как мы его испортим
    movq %rdi, 40(%rdi)
    movq %rbp, 48(%rdi)
    
    # Сохраняем R8-R15
    movq %r8,  56(%rdi)
    movq %r9,  64(%rdi)
    movq %r10, 72(%rdi)
    movq %r11, 80(%rdi)
    movq %r12, 88(%rdi)
    movq %r13, 96(%rdi)
    movq %r14, 104(%rdi)
    movq %r15, 112(%rdi)
    
    # Сохраняем RSP (указатель стека)
    # RSP сейчас указывает на адрес возврата (который на стеке)
    # Нам нужно сохранить RSP, который был ДО вызова context_switch
    # После выполнения call на стеке лежит адрес возврата
    # Поэтому оригинальный RSP = текущий RSP + 8
    movq %rsp, %rax
    addq $8, %rax
    movq %rax, 120(%rdi)   # rsp
    
    # Сохраняем RIP (адрес возврата со стека)
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
    # Теперь переключаемся на new_context (RSI)
    # ========================================
    
    # Восстанавливаем CR3 (если нужно переключить адресное пространство)
    movq %cr3, %rax
    cmpq %rax, 144(%rsi)
    je 1f
    movq 144(%rsi), %rax
    movq %rax, %cr3
1:
    
    # Восстанавливаем регистры общего назначения
    # Кроме RSI и RDI - их восстановим позже
    movq 0(%rsi), %rax
    movq 8(%rsi), %rbx
    movq 16(%rsi), %rcx
    movq 24(%rsi), %rdx
    movq 48(%rsi), %rbp
    
    # Восстанавливаем R8-R15
    movq 56(%rsi), %r8
    movq 64(%rsi), %r9
    movq 72(%rsi), %r10
    movq 80(%rsi), %r11
    movq 88(%rsi), %r12
    movq 96(%rsi), %r13
    movq 104(%rsi), %r14
    movq 112(%rsi), %r15
    
    # Восстанавливаем RFLAGS
    movq 136(%rsi), %rax
    pushq %rax
    popfq
    
    # Восстанавливаем RSP (переключаем стек!)
    movq 120(%rsi), %rsp
    
    # Теперь мы на новом стеке!
    # Сохраняем новый RIP во временный регистр
    movq 128(%rsi), %rax
    
    # Восстанавливаем RDI и RSI последними
    movq 40(%rsi), %rdi
    movq 32(%rsi), %rsi
    
    # Прыгаем на сохранённый RIP
    # RIP уже сохранён в RAX
    jmp *%rax

.size context_switch, .-context_switch