.section .text
.extern isr_common_handler

.global isr_common
.type isr_common, @function
isr_common:
    cld
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rdi
    pushq %rsi
    pushq %rbp
    pushq %rdx
    pushq %rcx
    pushq %rbx
    pushq %rax
    movq %rsp, %rdi
    call isr_common_handler
    popq %rax
    popq %rbx
    popq %rcx
    popq %rdx
    popq %rbp
    popq %rsi
    popq %rdi
    popq %r8
    popq %r9
    popq %r10
    popq %r11
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    addq $16, %rsp
    iretq

// Заглушка для неиспользуемых векторов (48-255)
.global isr_default
.type isr_default, @function
isr_default:
    iretq

.macro ISR_NOERR num
    .global isr\num
    .type isr\num, @function
isr\num:
    cli
    pushq $0
    pushq $\num
    jmp isr_common
.endm

.macro ISR_ERR num
    .global isr\num
    .type isr\num, @function
isr\num:
    cli
    pushq $\num
    jmp isr_common
.endm

// Исключения без кода ошибки
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_NOERR 9
ISR_NOERR 15
ISR_NOERR 16
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 31

// Исключения с кодом ошибки
ISR_ERR 8
ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14
ISR_ERR 17
ISR_ERR 21
ISR_ERR 29
ISR_ERR 30

// ================ НОВОЕ: IRQ 0–15 → векторы 32–47 ================
.macro IRQ num
    .global irq\num
    .type irq\num, @function
irq\num:
    cli
    pushq $0
    pushq $\num
    jmp isr_common
.endm

IRQ 32
IRQ 33
IRQ 34
IRQ 35
IRQ 36
IRQ 37
IRQ 38
IRQ 39
IRQ 40
IRQ 41
IRQ 42
IRQ 43
IRQ 44
IRQ 45
IRQ 46
IRQ 47

// ================ ТАБЛИЦА stubs (первые 256 векторов) ================
.section .rodata
.global isr_stub_table
.type isr_stub_table, @object
.align 8
isr_stub_table:
    .quad isr0
    .quad isr1
    .quad isr2
    .quad isr3
    .quad isr4
    .quad isr5
    .quad isr6
    .quad isr7
    .quad isr8
    .quad isr9
    .quad isr10
    .quad isr11
    .quad isr12
    .quad isr13
    .quad isr14
    .quad isr15
    .quad isr16
    .quad isr17
    .quad isr18
    .quad isr19
    .quad isr20
    .quad isr21
    .quad isr22
    .quad isr23
    .quad isr24
    .quad isr25
    .quad isr26
    .quad isr27
    .quad isr28
    .quad isr29
    .quad isr30
    .quad isr31

    // IRQ 0..15 → векторы 32..47
    .quad irq32
    .quad irq33
    .quad irq34
    .quad irq35
    .quad irq36
    .quad irq37
    .quad irq38
    .quad irq39
    .quad irq40
    .quad irq41
    .quad irq42
    .quad irq43
    .quad irq44
    .quad irq45
    .quad irq46
    .quad irq47

    // Остальные 208 векторов – заглушка
    .rept 208
        .quad isr_default
    .endr

.size isr_stub_table, .-isr_stub_table

.section .note.GNU-stack,"",@progbits