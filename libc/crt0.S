# crt0.S — точка входа для программ, слинкованных с libc LufiraOS.
#
# "and $-16, %rsp" обязателен: ядро не гарантирует выравнивание стека на
# входе (RSP приходит ≡ 8 (mod 16)), а SysV ABI перед call требует 16-байтного
# выравнивания.
#
# argc/argv/envp передаются ЧЕРЕЗ РЕГИСТРЫ (rdi/rsi/rdx), а не через стек, как
# в обычном Linux crt0 — возможно именно потому, что crt0.S тут под полным
# контролем ядра: build_exec_stack() (kernel/system/process/process.c) кладёт
# итоговые адреса + argc прямо в контекст процесса, а context_enter_ring3()
# (switch.S) restore'ит их в регистры непосредственно перед iretq. Между
# входом сюда и "call main" ни одна инструкция не трогает rdi/rsi/rdx/rax.
#
# Сборка программы против этой libc:
#   gcc -m64 -ffreestanding -fno-builtin -fno-pic -fno-pie -mgeneral-regs-only
#       -mno-red-zone -nostdlib -static -Ilibc/include -c <file.c/.S> -o <file.o>
#   (аналогично для program.c, libc/src/{string,malloc,printf}.c)
#   ld -m elf_x86_64 -static -nostdlib -no-pie -o program.elf \
#      crt0.o program.o string.o malloc.o printf.o
#
# -fno-pic/-fno-pie/-no-pie обязательны: elf_load_to_process() (kernel/system/
# elf/elf.c) грузит сегменты по их p_vaddr и прыгает на сырой entry без
# обработки релокаций — PIE-бинарник (ET_DYN) стартовал бы, но обращения
# через GOT/PLT читали бы мусор.
# -mgeneral-regs-only/-mno-red-zone — по той же причине, что и для самого
# ядра (см. switch.S): context_switch()/context_enter_ring3() не сохраняют
# XMM/FPU, а вытеснение может прервать пользовательский код на любой
# инструкции.

.section .text
.global _start
.extern main
.extern exit

_start:
    and  $-16, %rsp     # ядро не гарантирует выравнивание стека на входе
    xor  %ebp, %ebp      # стандартный терминатор для unwind/backtrace
    call main             # int main(int argc, char **argv, char **envp) —
                          # argc/argv/envp уже в rdi/rsi/rdx, см. комментарий выше
    mov  %eax, %edi        # exit(main())
    call exit

    # exit() оборачивает SYS_EXIT и не возвращается — это просто защита на
    # случай, если он всё же вернётся.
1:
    hlt
    jmp  1b

.section .note.GNU-stack,"",@progbits
