// argv_test.c — проверяет реальную передачу argc/argv/envp через
// crt0.S -> main() (kernel/system/process/process.c: build_exec_stack(),
// kernel/system/elf/elf.c: elf_exec()/elf_exec_replace()) — раньше ядро
// вообще не передавало аргументы командной строки ни в каком виде.
//
// Сборка — как у hello.c (см. header-комментарий там же).

#include <stdio.h>
#include <lufira/syscall.h>

int main(int argc, char **argv, char **envp) {
    printf("argc=%d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("argv[%d]=%s\n", i, argv[i]);
    }
    printf("envp-null=%d\n", envp == 0 || envp[0] == 0);
    printf("all tests done\n");
    return 0;
}
