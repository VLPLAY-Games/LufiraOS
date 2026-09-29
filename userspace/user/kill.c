// kill.c — вынос "kill" (kernel/shell/commands/system.c, command_kill()) в
// отдельную userspace-программу (v0.7 план, этап 5, под-этап 2).
//
// Сознательно НЕ 1:1 порт: kernel-native версия после SIGKILL/SIGTERM сама
// зовёт process_wait(pid, NULL), чтобы не оставлять зомби — работает
// только потому, что command_kill() исполняется прямо в процессе шелла,
// который И ЕСТЬ настоящий родитель pid, запущенного через run/runbg.
// process_wait()/SYS_WAIT (kernel/system/process/process.c) проверяет
// p->ppid == caller_pid — а kill.elf, запущенный через "run kill.elf <pid>",
// сам является ребёнком шелла, а не родителем killed-pid, так что его
// собственный sys_wait(pid) на этот pid всегда провалился бы ("not a
// child"). Поэтому здесь только сигнал — авто-reap убран как архитектурно
// невозможный для отдельного процесса; та же причина, по которой "wait" в
// принципе никогда не бывает отдельной программой ни в одном настоящем
// Unix-шелле (bash/dash/...), а только builtin'ом — wait.elf сюда
// сознательно НЕ добавлен, см. отчёт по этому под-этапу.
//
// atoi() нет в этой freestanding libc (stdlib.h — только malloc/free/exit) —
// ручной парсер, тот же приём, что уже у dlpg.c/parse_uint_local().

#include <stdio.h>
#include <string.h>
#include <lufira/syscall.h>

#define SIGKILL 9
#define SIGTERM 15
#define SIGCONT 18
#define SIGSTOP 19

static int parse_int(const char *s) {
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: kill [-SIGNAL] <pid>\n");
        printf("Signals: -TERM (default), -KILL, -STOP, -CONT (or numeric: -15, -9, -19, -18)\n");
        return 1;
    }

    int sig = SIGTERM;
    int idx = 1;

    // execute_command() лоуеркейсит всю командную строку до разбора
    // (kernel/shell/shell.c) ещё до того, как argv доходит до elf_exec() —
    // тот же приём, что уже описан в ls.c/is_executable_name(), так что
    // argv[1] сюда приходит уже в нижнем регистре, даже если пользователь
    // набрал "kill -KILL 3".
    if (argv[1][0] == '-') {
        const char *sig_arg = argv[1] + 1;
        if (strcmp(sig_arg, "kill") == 0) sig = SIGKILL;
        else if (strcmp(sig_arg, "term") == 0) sig = SIGTERM;
        else if (strcmp(sig_arg, "stop") == 0) sig = SIGSTOP;
        else if (strcmp(sig_arg, "cont") == 0) sig = SIGCONT;
        else {
            int n = parse_int(sig_arg);
            if (n <= 0) {
                printf("Unknown signal: %s\n", argv[1]);
                return 1;
            }
            sig = n;
        }
        idx = 2;
    }

    if (idx >= argc) {
        printf("Usage: kill [-SIGNAL] <pid>\n");
        return 1;
    }

    int pid = parse_int(argv[idx]);
    if (pid <= 0) {
        printf("Invalid PID\n");
        return 1;
    }

    long result = sys_kill(pid, sig);
    if (result == 0) {
        printf("Signal %d sent to PID %d\n", sig, pid);
        return 0;
    }
    printf("Process %d not found\n", pid);
    return 1;
}
