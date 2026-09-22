# crt0.S — точка входа для программ, слинкованных с libc LufiraOS.
#
# Ядро не даёт НИКАКИХ гарантий выравнивания стека на входе (stack_base в
# process_create() — это всегда кратное USER_STACK_SIZE=16384, минус 24
# байта под мёртвый scaffold, так что реальный RSP на входе ≡ 8 (mod 16),
# а не 16-выровнен, как того требует SysV ABI перед call) и не передаёт
# argc/argv/envp вообще — process_create() кладёт на стек только 3 никем не
# читаемых "мёртвых" qword'а (context_switch()/context_enter_ring3() берут
# RSP/RIP прямо из полей структуры контекста, а не снимают их со стека).
# Поэтому main() здесь объявлен как int main(void) — обещать argc/argv,
# которых ядро не даёт, было бы враньём в заголовке.
#
# Сборка программы против этой libc (пример):
#   gcc -m64 -ffreestanding -fno-builtin -fno-pic -fno-pie \
#       -mgeneral-regs-only -mno-red-zone -nostdlib -static -Wall -Wextra \
#       -Ilibc/include -c crt0.S -o crt0.o
#   gcc <тем же флагами> -c program.c -o program.o
#   gcc <тем же флагами> -c libc/src/string.c -o string.o
#   gcc <тем же флагами> -c libc/src/malloc.c -o malloc.o
#   gcc <тем же флагами> -c libc/src/printf.c -o printf.o
#   ld -m elf_x86_64 -static -nostdlib -no-pie \
#      -o program.elf crt0.o program.o string.o malloc.o printf.o
#
# -fno-pic/-fno-pie/-no-pie обязательны: GCC на современных дистрибутивах по
# умолчанию собирает PIE (ET_DYN) — elf_load_to_process() (kernel/system/
# elf/elf.c) грузит сегменты по их же p_vaddr и прыгает на сырой entry БЕЗ
# КАКОЙ-ЛИБО обработки релокаций, так что PIE-бинарник загрузился бы и
# стартовал, но любое обращение через GOT/PLT читало бы мусор.
# -mgeneral-regs-only/-mno-red-zone обязательны по той же причине, что и для
# самого ядра (см. kernel/system/process/switch.S): context_switch()/
# context_enter_ring3() не сохраняют XMM/FPU вообще, а вытеснение (см. план
# v0.4 Phase 1) может прервать пользовательский код на любой инструкции.

.section .text
.global _start
.extern main
.extern exit

_start:
    and  $-16, %rsp     # ядро не гарантирует выравнивание стека на входе
    xor  %ebp, %ebp      # стандартный терминатор для unwind/backtrace
    call main             # int main(void) — argc/argv ядро не передаёт
    mov  %eax, %edi        # exit(main())
    call exit

    # exit() оборачивает SYS_EXIT и не возвращается — это просто защита на
    # случай, если он всё же вернётся.
1:
    hlt
    jmp  1b

.section .note.GNU-stack,"",@progbits
