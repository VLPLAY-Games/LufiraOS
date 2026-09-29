// free.c — вынос команды "free" (kernel/shell/commands/system.c,
// command_free()) в отдельную userspace-программу поверх нового
// SYS_MEMINFO (v0.7, план, этап 1). Ни одного нового примитива в самой
// программе — просто буфер вместо printf() внутри ядра.
//
// Сборка — как у test/c/hello.c (см. header-комментарий в libc/crt0.S):
//   gcc $FLAGS -c userspace/user/free.c -o free.o
//   ld -m elf_x86_64 -static -nostdlib -no-pie -o free.elf
//      crt0.o free.o string.o malloc.o printf.o stdlib.o

#include <stdio.h>
#include <lufira/syscall.h>

int main(void) {
    struct lufira_meminfo mi;
    long r = sys_meminfo(&mi);
    if (r != 0) {
        printf("free: SYS_MEMINFO failed (%ld)\n", r);
        return 1;
    }

    unsigned long total_mb = (mi.total_pages * 4) / 1024;
    unsigned long used_mb = (mi.used_pages * 4) / 1024;
    unsigned long free_mb = ((mi.total_pages - mi.used_pages) * 4) / 1024;

    // kernel-printf (и этот, userspace) не понимает ширину поля (%8lu) —
    // тот же предел, что уже отмечен у command_free(), см. system.c.
    printf("RAM:  %lu MB total, %lu MB used, %lu MB free\n", total_mb, used_mb, free_mb);
    printf("Heap: %lu KB total, %lu KB used, %lu KB free\n",
           mi.heap_total_bytes / 1024, mi.heap_used_bytes / 1024,
           (mi.heap_total_bytes - mi.heap_used_bytes) / 1024);
    return 0;
}
