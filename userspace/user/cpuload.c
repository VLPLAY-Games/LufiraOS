// cpuload.c — вынос команды "cpuload" (kernel/shell/commands/system.c,
// command_cpuload()) в отдельную userspace-программу поверх SYS_CPULOAD
// (v0.7 план, этап 1). Per-process разбивка добавлена здесь же, в этапе 5
// под-этапе 4, как только появился SYS_PSLIST (тот же снимок pid+cpu_ticks,
// что уже берёт kernel-native command_cpuload() напрямую из process_list).
//
// Сэмплирование — как у kernel-native версии: два снимка (общих тиков И
// per-process pslist) с sys_msleep() между ними, % считается здесь же.
//
// Сборка — как у free.c/hello.c.

#include <stdio.h>
#include <lufira/syscall.h>

#define CPULOAD_SAMPLE_MS 500
#define MAX_PROCESSES 32

int main(void) {
    struct lufira_cpuload before, after;
    struct lufira_ps_entry procs_before[MAX_PROCESSES];
    struct lufira_ps_entry procs_after[MAX_PROCESSES];

    long r = sys_cpuload(&before);
    if (r != 0) {
        printf("cpuload: SYS_CPULOAD failed (%ld)\n", r);
        return 1;
    }
    long count_before = sys_pslist(procs_before, MAX_PROCESSES);
    if (count_before < 0) count_before = 0;

    sys_msleep(CPULOAD_SAMPLE_MS);

    r = sys_cpuload(&after);
    if (r != 0) {
        printf("cpuload: SYS_CPULOAD failed (%ld)\n", r);
        return 1;
    }
    long count_after = sys_pslist(procs_after, MAX_PROCESSES);
    if (count_after < 0) count_after = 0;

    unsigned long total_delta = (unsigned long)(after.total_ticks - before.total_ticks);
    unsigned long idle_delta = (unsigned long)(after.idle_ticks - before.idle_ticks);
    unsigned long pct = total_delta ? (100 - (idle_delta * 100 / total_delta)) : 0;

    printf("CPU load: %lu%%\n\n", pct);
    printf("PID  NAME  CPU%%\n");
    for (long i = 0; i < count_after; i++) {
        unsigned long ticks_before = 0;
        for (long j = 0; j < count_before; j++) {
            if (procs_before[j].pid == procs_after[i].pid) {
                ticks_before = (unsigned long)procs_before[j].cpu_ticks;
                break;
            }
        }
        unsigned long delta = (unsigned long)procs_after[i].cpu_ticks - ticks_before;
        unsigned long proc_pct = total_delta ? (delta * 100 / total_delta) : 0;
        printf("%u  %s  %lu%%\n", procs_after[i].pid, procs_after[i].name, proc_pct);
    }
    return 0;
}
