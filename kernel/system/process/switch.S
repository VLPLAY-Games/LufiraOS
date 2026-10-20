.section .text
.extern current_kernel_rsp
.global context_switch
.type context_switch, @function

# ============================================================
# void context_switch(context_t* old, context_t* new)
#
# rdi = old context
# rsi = new context
#
# Context layout:
#
#   0   rax
#   8   rbx
#   16  rcx
#   24  rdx
#   32  rsi
#   40  rdi
#   48  rbp
#   56  r8
#   64  r9
#   72  r10
#   80  r11
#   88  r12
#   96  r13
#   104 r14
#   112 r15
#   120 rsp
#   128 rip
#   136 rflags
#   144 cr3
#
# Requirements:
# - Must be called via normal CALL instruction
# - Kernel code/data/stacks must be mapped in all CR3s
# - Interrupts should already be disabled before calling
# ============================================================

context_switch:

    # ========================================================
    # SAVE CURRENT CONTEXT
    # ========================================================

    movq %rax,   0(%rdi)
    movq %rbx,   8(%rdi)
    movq %rcx,  16(%rdi)
    movq %rdx,  24(%rdi)
    movq %rsi,  32(%rdi)
    movq %rdi,  40(%rdi)
    movq %rbp,  48(%rdi)

    movq %r8,   56(%rdi)
    movq %r9,   64(%rdi)
    movq %r10,  72(%rdi)
    movq %r11,  80(%rdi)
    movq %r12,  88(%rdi)
    movq %r13,  96(%rdi)
    movq %r14, 104(%rdi)
    movq %r15, 112(%rdi)

    # Save RIP (return address from CALL)
    movq (%rsp), %rax
    movq %rax, 128(%rdi)

    # Save RSP after return address
    leaq 8(%rsp), %rax
    movq %rax, 120(%rdi)

    # Save RFLAGS
    pushfq
    popq %rax
    movq %rax, 136(%rdi)

    # Save CR3
    movq %cr3, %rax
    movq %rax, 144(%rdi)

    # ========================================================
    # RESTORE NEXT CONTEXT
    # ========================================================

    # Switch address space first
    movq 144(%rsi), %rax
    movq %rax, %cr3

    # Restore stack pointer
    movq 120(%rsi), %rsp

    # Load target RIP into scratch register
    # r11 is safe because old r11 already saved
    movq 128(%rsi), %r11

    # ========================================================
    # Restore registers
    # ========================================================

    movq   0(%rsi), %rax
    movq   8(%rsi), %rbx
    movq  16(%rsi), %rcx
    movq  24(%rsi), %rdx

    movq  48(%rsi), %rbp

    movq  56(%rsi), %r8
    movq  64(%rsi), %r9
    movq  72(%rsi), %r10

    # DO NOT restore r11
    # r11 currently contains RIP target

    movq  88(%rsi), %r12
    movq  96(%rsi), %r13
    movq 104(%rsi), %r14
    movq 112(%rsi), %r15

    # Restore RDI before RSI
    movq 40(%rsi), %rdi

    # RFLAGS пушится прямо из памяти, НЕ через %rax как временный регистр:
    # %rax уже восстановлен в реальное значение контекста (0(%rsi), для
    # только что forked ребёнка это возвращаемое значение fork()) —
    # маршрутизация через него затёрла бы это значение перед jmp ниже.
    pushq 136(%rsi)

    # Restore RSI LAST
    movq 32(%rsi), %rsi

    # Restore RFLAGS as late as possible
    popfq

    # Jump to restored RIP
    jmp *%r11

.size context_switch, .-context_switch

# ============================================================
# void context_enter_ring3(process_context_t* old, process_context_t* new)
#
# Как context_switch() (идентичная половина SAVE), но RESTORE делает
# настоящий переход ring0->ring3 через iretq вместо jmp на том же уровне
# привилегий. Используется ровно раз на процесс: при самой первой активации
# (process_t.first_run) или после exec() (elf_exec_replace(), elf.c) — оба
# случая это "первая инструкция новой программы" (CS/SS те же 0x33/0x2B,
# что и в syscall_entry.S). Трюк с r11 как промежуточным регистром здесь не
# нужен: iretq берёт готовый кадр со стека, а не прыгает через регистр.
# ============================================================

.global context_enter_ring3
.type context_enter_ring3, @function

context_enter_ring3:

    # ========================================================
    # SAVE CURRENT CONTEXT — byte-identical to context_switch()'s save half
    # ========================================================

    movq %rax,   0(%rdi)
    movq %rbx,   8(%rdi)
    movq %rcx,  16(%rdi)
    movq %rdx,  24(%rdi)
    movq %rsi,  32(%rdi)
    movq %rdi,  40(%rdi)
    movq %rbp,  48(%rdi)

    movq %r8,   56(%rdi)
    movq %r9,   64(%rdi)
    movq %r10,  72(%rdi)
    movq %r11,  80(%rdi)
    movq %r12,  88(%rdi)
    movq %r13,  96(%rdi)
    movq %r14, 104(%rdi)
    movq %r15, 112(%rdi)

    movq (%rsp), %rax
    movq %rax, 128(%rdi)

    leaq 8(%rsp), %rax
    movq %rax, 120(%rdi)

    pushfq
    popq %rax
    movq %rax, 136(%rdi)

    movq %cr3, %rax
    movq %rax, 144(%rdi)

    # ========================================================
    # RESTORE NEXT CONTEXT — into ring 3 via iretq
    # ========================================================

    # Switch address space first
    movq 144(%rsi), %rax
    movq %rax, %cr3

    # Переключаемся на СОБСТВЕННЫЙ ring0-стек целевого процесса ДО постройки
    # кадра — current_kernel_rsp уже выставлен в next->ring0_stack
    # switch_to_process() непосредственно перед этим вызовом (тот же стек,
    # на который переключается syscall_entry.S при входе в syscall). Это не
    # опционально: %rsp на входе принадлежит ВЫЗВАВШЕМУ (стек предыдущего
    # процесса, уже сохранённый выше в *rdi) и замаплен только в СТАРОЙ
    # таблице страниц — построение кадра на нём после смены CR3 дало бы page
    # fault. ring0_stack гарантированно замаплен в НОВОМ cr3
    # (allocate_ring0_stack() маппит его именно туда для этого процесса).
    movq current_kernel_rsp(%rip), %rsp

    # Build the iretq frame on that stack — transient scratch space,
    # abandoned the instant iretq fires below (nothing ever reads it again
    # after that). Order matters: SS/RSP/RFLAGS/CS/RIP, pushed high-to-low
    # address so iretq pops RIP first.
    pushq $0x2B            # user SS (data)
    pushq 120(%rsi)         # user RSP
    pushq 136(%rsi)         # RFLAGS
    pushq $0x33             # user CS (code)
    pushq 128(%rsi)         # RIP

    # Load GP regs from *new. All reads happen before %rsi itself is
    # overwritten (loaded last, same convention as context_switch()).
    movq   8(%rsi), %rbx
    movq  16(%rsi), %rcx
    movq  24(%rsi), %rdx
    movq  48(%rsi), %rbp
    movq  56(%rsi), %r8
    movq  64(%rsi), %r9
    movq  72(%rsi), %r10
    movq  80(%rsi), %r11
    movq  88(%rsi), %r12
    movq  96(%rsi), %r13
    movq 104(%rsi), %r14
    movq 112(%rsi), %r15
    movq  40(%rsi), %rdi
    movq   0(%rsi), %rax
    movq  32(%rsi), %rsi

    iretq

.size context_enter_ring3, .-context_enter_ring3

.section .note.GNU-stack,"",@progbits