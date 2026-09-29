// cpuload.c — вынос команды "cpuload" (kernel/shell/commands/system.c,
// command_cpuload()) в отдельную userspace-программу поверх нового
// SYS_CPULOAD (v0.7, план, этап 1). Только системная доля — kernel-native
// версия дополнительно печатает разбивку по процессам, для которой нужен
// отдельный syscall (SYS_PSLIST, запланирован на этап 5 вместе с "ps") —
// per-process строки сюда добавятся, когда он появится.
//
// Сэмплирование — как у kernel-native версии: два снимка сырых тиков с
// sys_msleep() между ними, % считается здесь же, в userspace.
//
// Сборка — как у free.c/hello.c.

#include <stdio.h>
#include <lufira/syscall.h>

#define CPULOAD_SAMPLE_MS 500

int main(void) {
    struct lufira_cpuload before, after;

    long r = sys_cpuload(&before);
    if (r != 0) {
        printf("cpuload: SYS_CPULOAD failed (%ld)\n", r);
        return 1;
    }

    sys_msleep(CPULOAD_SAMPLE_MS);

    r = sys_cpuload(&after);
    if (r != 0) {
        printf("cpuload: SYS_CPULOAD failed (%ld)\n", r);
        return 1;
    }

    unsigned long total_delta = (unsigned long)(after.total_ticks - before.total_ticks);
    unsigned long idle_delta = (unsigned long)(after.idle_ticks - before.idle_ticks);
    unsigned long pct = total_delta ? (100 - (idle_delta * 100 / total_delta)) : 0;

    printf("CPU load: %lu%%\n", pct);
    return 0;
}
