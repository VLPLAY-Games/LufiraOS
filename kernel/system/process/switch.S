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

    # Prepare RFLAGS restore value. Pushed straight from memory (NOT via
    # %rax as a scratch register, like before) — %rax was just restored to
    # its real context value a few lines up (0(%rsi)), and routing RFLAGS
    # through it here clobbered that value right before the jmp below. For
    # a freshly-forked child this %rax IS the fork() return value (0, so it
    # takes the "child" branch) — with the clobber it silently became
    # whatever RFLAGS happened to be (always non-zero, e.g. 0x202+), so the
    # child always fell through to the "parent" branch instead ("Parent
    # process here!" printed twice, "Child process here!" never printed).
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
# Same as context_switch() (identical SAVE half, same layout above), except
# the RESTORE half performs a genuine ring0->ring3 transition via iretq
# instead of a same-privilege jmp. Used exactly once per process: the very
# first time it's ever activated (process_t.first_run in process.c), or
# whenever exec() replaces a process's image (elf_exec_replace() in elf.c) —
# both cases are "a new program's first instruction," which today ran with
# kernel privilege until the process's first syscall RETURN (the only other
# place in the kernel that does a real iretq back to ring3 is
# syscall_entry.S — same CS/SS selectors, 0x33/0x2B, reused here).
#
# No r11-as-jump-target dance needed here (unlike context_switch): nothing
# below needs a scratch register protected across the final transfer, since
# the transfer is iretq consuming a frame already built on the stack, not a
# jmp through a register.
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

    # Switch to the TARGET process's own ring0 kernel stack before building
    # anything — current_kernel_rsp was just set to next->ring0_stack by
    # switch_to_process() right before this call, same global
    # syscall_entry.S itself swaps onto on syscall entry. This is NOT
    # optional: %rsp at function entry belongs to whoever CALLED us (e.g.
    # the previous process's own stack, already fully captured above into
    # *rdi and no longer needed) — that stack is mapped only in the OLD
    # process's page table, not the one we just switched CR3 into, so
    # building the iretq frame there after the cr3 switch page-faults
    # (confirmed live: supervisor write, not-present, on what turned out to
    # be the caller's stack — this exact bug happened during development).
    # ring0_stack, by contrast, is guaranteed mapped in the NEW cr3, because
    # allocate_ring0_stack() mapped it there specifically for this process.
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