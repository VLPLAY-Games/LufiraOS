// ps.c — вынос "ps" (kernel/system/process/process.c, process_ps()) в
// отдельную userspace-программу (v0.7 план, этап 5, под-этап 4).
//
// Единственная команда во всём этапе 5, которой реально не хватало
// syscall'а: раньше не было вообще никакого способа получить список ВСЕХ
// процессов из userspace (только SYS_WAIT на собственных детей) — добавлен
// SYS_PSLIST (30), плоский снимок process_list одним вызовом (см.
// process_pslist(), process.c). Буфер на MAX_PROCESSES (32, process.h) —
// столько процессов в этой ОС одновременно быть не может, так что второй
// проход "буфер оказался мал" не нужен.

#include <stdio.h>
#include <lufira/syscall.h>

#define MAX_PROCESSES 32

static const char *state_name(uint32_t state) {
    switch (state) {
        case LUFIRA_PROCESS_READY:      return "READY";
        case LUFIRA_PROCESS_RUNNING:    return "RUNNING";
        case LUFIRA_PROCESS_BLOCKED:    return "BLOCKED";
        case LUFIRA_PROCESS_SLEEPING:   return "SLEEPING";
        case LUFIRA_PROCESS_TERMINATED: return "TERMINATED";
        case LUFIRA_PROCESS_STOPPED:    return "STOPPED";
        default:                        return "UNKNOWN";
    }
}

int main(void) {
    struct lufira_ps_entry procs[MAX_PROCESSES];
    long count = sys_pslist(procs, MAX_PROCESSES);
    if (count < 0) {
        printf("ps: failed to read process list\n");
        return 1;
    }

    printf("PID   STATE       NAME\n");
    printf("-------------------------------\n");
    if (count == 0) {
        printf("No processes\n");
        return 0;
    }

    for (long i = 0; i < count; i++) {
        printf("%u     %s     %s\n", procs[i].pid, state_name(procs[i].state), procs[i].name);
    }
    return 0;
}
