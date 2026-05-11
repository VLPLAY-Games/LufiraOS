.section .text
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

    # Prepare RFLAGS restore value
    movq 136(%rsi), %rax
    pushq %rax

    # Restore RSI LAST
    movq 32(%rsi), %rsi

    # Restore RFLAGS as late as possible
    popfq

    # Jump to restored RIP
    jmp *%r11

.size context_switch, .-context_switch