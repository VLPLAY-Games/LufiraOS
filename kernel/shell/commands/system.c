#include "lib/string.h"
#include "lib/cpu.h"
#include "../commands.h"
#include "drivers/console/console.h"
#include "../shell.h"
#include "fs/lufirafs/lufirafs.h"
#include "system/acpi/acpi.h"
#include "system/process/process.h"
#include "system/mm/heap.h"
#include "system/elf/elf.h"
#include "system/devmode/devmode.h"
#include "system/klog/klog.h"
#include "system/mm/pmm.h"
#include "system/timer/pit.h"

extern lufirafs_t lufirafs;
// cwd_inode теперь макрос поверх current_process->cwd_inode (shell.h).

// help, clear, reboot, shutdown, version, status, trap
void command_help(void) {
    printf("\nAvailable commands (any command taking arguments also accepts -help/--help/-h):\n");
    printf(" help - Show this help\n");
    printf(" clear - Clear screen\n");
    printf(" reboot - Reboot system\n");
    printf(" shutdown - Shutdown system\n");
    printf(" version - Show kernel version\n");
    printf(" echo - Echo text back\n");
    printf(" history - Show command history\n");
    printf(" status - Show interrupt/CPU status\n");
    printf(" free - Show physical RAM and kernel heap usage\n");
    printf(" cpuload - Show system and per-process CPU load\n");
    printf(" trap - Trigger test exceptions\n");
    printf(" color - Set console colors or reset\n");
    printf(" colors - Show available colors\n");
    printf(" fg <color> - Set foreground color\n");
    printf(" bg <color> - Set background color\n");
    printf("\nFile system commands:\n");
    printf(" pwd - Print current directory\n");
    printf(" cd <dir> - Change directory\n");
    printf(" ls [-l] [path] - List directory contents\n");
    printf(" mkdir <name> - Create directory\n");
    printf(" rm <name> - Remove file/directory\n");
    printf(" cp <src> <dst> - Copy file\n");
    printf(" mv <src> <dst> - Move/rename file\n");
    printf(" cat <file> - Display file content\n");
    printf(" touch <filename> - Create empty file\n");
    printf(" write <file> <text> - Write text to file\n");
    printf(" edit <file> <text> - Append text to file\n");
    printf(" df - Show filesystem free/used space\n");
    printf(" du [path] - Show disk usage of a file/directory\n");
    printf(" run <file> - Execute ELF program\n");
    printf(" runbg <file> - Execute ELF program in background\n");
    printf(" kill [-SIGNAL] <pid> - Send a signal (-TERM/-KILL/-STOP/-CONT, default -TERM)\n");
    printf(" wait <pid> - Wait for a child process to exit\n");
    printf(" beep - Play beep to check sound\n");
    printf(" music - Play sample music to check sound\n");
    printf(" mixer <volume> - Change sound volume\n");
    printf(" exec <file> - Replace current process with ELF program\n");
    printf(" ps - List running processes\n");
    printf(" devmode [on|off] - Show/toggle developer mode (verbose driver output)\n");
    printf("\nUsers & permissions:\n");
    printf(" whoami - Show current user/group\n");
    printf(" chmod <mode> <path> - Change file permissions (octal, e.g. 644)\n");
    printf(" chown <user>[:group] <path> - Change file owner (root only)\n");
    printf(" useradd <user> <password> [group] - Create a new user\n");
    printf(" groupadd <group> - Create a new group\n");
    printf(" su <user> [password] - Switch user\n");
    printf(" passwd <new-password> - Change your own password\n");
    printf(" passwd -u <user> <new-password> - Reset another user's password (root only)\n");
    printf("\nUSB mass storage:\n");
    printf(" usbinfo - List detected USB storage devices\n");
    printf(" usbread <device> <lba> - Read one block, show hex dump\n");
    printf(" usbwrite <device> <lba> <text> - Write text into one block\n");
    printf(" mount [device] [name] - Mount a FAT filesystem from a USB device (no args lists mounts)\n");
    printf(" unmount [name] - Unmount a filesystem (up to 2 concurrent mounts)\n");
    printf(" mountls <name> - List root directory of a mount\n");
    printf(" mountcat <name> <file> - Print a file's contents from a mount's root\n");
    printf(" mountwrite <name> <file> <text> - Create/overwrite a file in a mount's root\n");
    printf("\nNetwork:\n");
    printf(" ifconfig [ip] [netmask] [gateway] - Show/set network configuration\n");
    printf(" ping <ip> [count] - Send ICMP echo requests\n");
    printf(" wget <ip> <path> [file] - Download a file over HTTP (IP only, no DNS)\n");
}

// devmode [on|off] — без аргументов показывает текущее состояние.
// Флаг персистентный (файл /system/devmode.flag на LufiraFS, см. devmode.h),
// поэтому переживает reboot. Все события всё равно попадают в
// /logs/system.log независимо от режима — см. klog.h.
void command_devmode(const char *args) {
    if (!args || *args == '\0') {
        printf("\nDeveloper mode: %s\n", devmode_is_enabled() ? "ON" : "OFF");
        printf("Usage: devmode <on|off>\n");
        return;
    }

    if (token_equals(args, "on")) {
        if (devmode_set(1) == 0) printf("\nDeveloper mode: ON\n");
        else printf("\nFailed to enable developer mode\n");
    } else if (token_equals(args, "off")) {
        if (devmode_set(0) == 0) printf("\nDeveloper mode: OFF\n");
        else printf("\nFailed to disable developer mode\n");
    } else {
        printf("\nUsage: devmode <on|off>\n");
    }
}

void command_clear(void) { clear_screen(); show_prompt(); }
void command_reboot(void) {
    printf("\nSyncing filesystem... ");
    lufirafs_flush(&lufirafs);
    printf("done.\nRebooting system...\n");
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    printf("Reboot failed. Please restart manually.\n");
}
void command_shutdown(void) {
    printf("\nSyncing filesystem... ");
    lufirafs_flush(&lufirafs);
    printf("done.\nShutting down system...\n");
    
    // Используем ACPI shutdown
    acpi_shutdown();
    
    // Если ACPI не сработал, пробуем legacy
    printf("Shutdown command sent. System may require manual power off.\n");
}
void command_version(void) {
    printf("\nLufiraOS Kernel v0.8.0\nBuilt: %s %s\nArchitecture: x86_64\n", __DATE__, __TIME__);
}
void command_status(void) {
    printf("\nSYSTEM STATUS:\n");
    printf("--------------\n");
    printf(" Interrupt Flag: %s\n", interrupts_enabled() ? "SET" : "CLEAR");
    printf(" Interrupts: %s\n", cpu_interrupts_active() ? "ENABLED" : "DISABLED");
    printf(" CPU Test: trap int3 / ud2 / pf\n");
}

// free — физическая RAM (pmm.c) + куча ядра (heap.c). Оба источника уже
// ведут живой учёт (used_pages/heap-список), тут только форматированный вывод.
void command_free(void) {
    uint64_t total_pages = pmm_get_total_pages();
    uint64_t used_pages = pmm_get_used_pages();
    uint64_t free_pages = total_pages - used_pages;

    uint64_t heap_used, heap_free;
    heap_get_stats(&heap_used, &heap_free);

    // Примечание: kernel-printf (console.c) не понимает ширину поля
    // (%8lu и т.п.) — неизвестный спецификатор просто пропускается, а
    // хвост печатается как обычный текст. Поэтому тут только %lu/%s без
    // выравнивания в столбец.
    printf("\nRAM:  %lu MB total, %lu MB used, %lu MB free\n",
           (unsigned long)(total_pages * 4 / 1024),
           (unsigned long)(used_pages * 4 / 1024),
           (unsigned long)(free_pages * 4 / 1024));
    printf("Heap: %lu KB total, %lu KB used, %lu KB free\n",
           (unsigned long)((heap_used + heap_free) / 1024),
           (unsigned long)(heap_used / 1024),
           (unsigned long)(heap_free / 1024));
}

// cpuload — сэмплирует idle/total тики PIT (pit.c) и cpu_ticks каждого
// процесса (process.h) до и после короткой паузы, печатает загрузку в
// процентах системно и по процессам. Пауза сделана через process_sleep()
// (настоящий yield планировщику — см. process.c), а НЕ через pit_wait_ms()
// (busy-hlt-цикл): pit_wait_ms() не меняет current_process, так что сам
// шелл всё это время оставался бы "текущим" и cpuload намеряла бы себе же
// 100% просто за факт своего собственного ожидания.
#define CPULOAD_SAMPLE_MS 500
void command_cpuload(void) {
    uint64_t total_before = pit_get_total_ticks();
    uint64_t idle_before = pit_get_idle_ticks();

    uint32_t pids[MAX_PROCESSES];
    uint64_t ticks_before[MAX_PROCESSES];
    int count = 0;

    process_t *p = process_list;
    if (p) {
        process_t *start = p;
        do {
            if (count < MAX_PROCESSES) {
                pids[count] = p->pid;
                ticks_before[count] = p->cpu_ticks;
                count++;
            }
            p = p->next;
        } while (p && p != start);
    }

    process_sleep(CPULOAD_SAMPLE_MS);

    uint64_t total_delta = pit_get_total_ticks() - total_before;
    uint64_t idle_delta = pit_get_idle_ticks() - idle_before;
    uint32_t system_pct = total_delta ? (uint32_t)(100 - (idle_delta * 100 / total_delta)) : 0;

    printf("\nCPU load: %u%%\n\n", system_pct);
    printf("PID  NAME  CPU%%\n");

    p = process_list;
    if (p) {
        process_t *start = p;
        do {
            uint64_t before = 0;
            for (int i = 0; i < count; i++) {
                if (pids[i] == p->pid) { before = ticks_before[i]; break; }
            }
            uint64_t delta = p->cpu_ticks - before;
            uint32_t pct = total_delta ? (uint32_t)(delta * 100 / total_delta) : 0;
            printf("%u  %s  %u%%\n", p->pid, p->name, pct);
            p = p->next;
        } while (p && p != start);
    }
}

void command_trap(void) {
    char* args = (char*)skip_spaces(input_buffer + 5);
    if (*args == '\0') { printf("\nUsage: trap <int3|ud2|pf|cli|sti|hlt>\n"); return; }
    if (token_equals(args, "int3")) { printf("\nTriggering breakpoint...\n"); asm volatile ("int3"); }
    else if (token_equals(args, "ud2")) { printf("\nTriggering invalid opcode...\n"); asm volatile ("ud2"); }
    else if (token_equals(args, "pf")) { printf("\nTriggering page fault...\n"); volatile uint64_t* bad = (volatile uint64_t*)0x0; *bad = 0xDEADBEEF; }
    else if (token_equals(args, "cli")) { asm volatile ("cli"); printf("\nInterrupt Flag cleared.\n"); }
    else if (token_equals(args, "sti")) { asm volatile ("sti"); printf("\nInterrupt Flag set.\n(All IRQs are masked by PIC)\n"); }
    else if (token_equals(args, "hlt")) { printf("\nHalting CPU.\n"); asm volatile ("hlt"); }
    else printf("\nUnknown trap: %s\n", args);
}

void command_echo(const char* args) {
    if (*args == '\0') printf("\nUsage: echo <text>\n");
    else printf("\n%s\n", args);
}

void command_runbg(const char *raw_args) {
    if (!raw_args || *raw_args == '\0') {
        printf("\nUsage: runbg <filename> [args...]\n");
        printf("Example: runbg hello.elf\n");
        return;
    }

    // Копия — split_argv() режет на месте, raw_args обычно указывает прямо
    // в сырой input_buffer шелла (см. shell.c).
    char buf[INPUT_BUFFER_SIZE];
    int i = 0;
    for (; raw_args[i] && i < INPUT_BUFFER_SIZE - 1; i++) buf[i] = raw_args[i];
    buf[i] = '\0';

    char *argv[MAX_EXEC_ARGS + 1];
    int argc = split_argv(buf, argv, MAX_EXEC_ARGS);
    if (argc == 0) {
        printf("\nUsage: runbg <filename> [args...]\n");
        return;
    }
    const char *filename = argv[0];

    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, cwd_inode, filename, &ino) != 0) {
        printf("\nFile not found: %s\n", filename);
        return;
    }

    lufirafs_inode_t inode;
    lufirafs_read_inode(&lufirafs, ino, &inode);
    uint32_t fsize = inode.size;

    uint8_t *file_buf = (uint8_t *)kmalloc(fsize);
    if (!file_buf) {
        printf("\nNot enough memory to load %s (%u bytes)\n",
               filename,
               fsize);
        return;
    }

    int br = lufirafs_read(&lufirafs, ino, 0, file_buf, fsize);
    if (br <= 0) {
        printf("\nError reading file: %s\n", filename);
        kfree(file_buf);
        return;
    }

    DLOG("\nLoading ELF in background: %s (%u bytes)...\n",
           filename,
           fsize);
    klog("[SHELL] runbg '%s' (%u bytes)", filename, fsize);

    // argv одалживается (elf_exec_background() его не освобождает, см.
    // elf.h) — указывает на локальный buf[], живой на всё время этого
    // синхронного вызова.
    int pid = elf_exec_background(file_buf, fsize, filename, argv, NULL);
    if (pid < 0) {
        printf("Failed to start background process\n");
        kfree(file_buf);
        return;
    }

    printf("Started background process PID %u\n", (uint32_t)pid);
    // elf_exec_background освободит буфер при успехе
}

void command_kill(const char *args)
{
    if (!args || *args == '\0') {
        printf("\nUsage: kill [-SIGNAL] <pid>\n");
        printf("Signals: -TERM (default), -KILL, -STOP, -CONT (or numeric: -15, -9, -19, -18)\n");
        printf("Example: kill -STOP 3\n");
        return;
    }

    int sig = SIGTERM;
    const char *pid_str = args;

    if (*args == '-') {
        const char *sig_arg = args + 1;
        int len = token_length(sig_arg);

        // execute_command() лоуеркейсит всю строку команды до разбора, так
        // что sig_arg сюда всегда приходит нижним регистром (даже если
        // пользователь набрал "kill -KILL 3") — сравниваем с ним же.
        if (token_equals(sig_arg, "kill")) sig = SIGKILL;
        else if (token_equals(sig_arg, "term")) sig = SIGTERM;
        else if (token_equals(sig_arg, "stop")) sig = SIGSTOP;
        else if (token_equals(sig_arg, "cont")) sig = SIGCONT;
        else {
            int n = atoi(sig_arg);
            if (n <= 0) {
                printf("\nUnknown signal: %s\n", args);
                return;
            }
            sig = n;
        }

        pid_str = skip_spaces(sig_arg + len);
    }

    if (*pid_str == '\0') {
        printf("\nUsage: kill [-SIGNAL] <pid>\n");
        return;
    }

    int pid = atoi(pid_str);

    if (pid <= 0) {
        printf("\nInvalid PID\n");
        return;
    }

    int result = process_signal((uint32_t)pid, sig);

    if (result == 0) {
        printf("Signal %d sent to PID %d\n", sig, pid);

        // SIGKILL/SIGTERM переводят процесс в PROCESS_TERMINATED, но сам по
        // себе он остаётся "зомби" в списке (и в выводе ps) до тех пор,
        // пока родитель не заберёт его через process_wait() — это
        // нормальная UNIX-семантика, но в этом шелле нет отдельного
        // авто-reap'а для процессов, о которых никто явно не спросит через
        // "wait". Раз уж мы (шелл, реальный родитель run/runbg) сами только
        // что синхронно попросили процесс завершиться — тут же и забираем
        // его, чтобы "kill" ощущался как окончательное действие, а не как
        // полдела, требующее ещё и "wait <pid>" вручную.
        if (sig == SIGKILL || sig == SIGTERM) {
            process_wait((uint32_t)pid, NULL);
        }
    } else {
        printf("Process %d not found\n", pid);
    }
}

// wait <pid> - блокируется, пока указанный (свой) ребёнок не завершится
void command_wait(const char *args)
{
    if (!args || *args == '\0') {
        printf("\nUsage: wait <pid>\n");
        printf("Example: wait 2\n");
        return;
    }

    int pid = atoi(args);

    if (pid <= 0) {
        printf("\nInvalid PID\n");
        return;
    }

    printf("\nWaiting for PID %d...\n", pid);

    int status = 0;
    int result = process_wait((uint32_t)pid, &status);

    if (result < 0) {
        printf("PID %d is not a child of this shell (or was already reaped)\n", pid);
    } else {
        printf("PID %d exited with code %d\n", result, status);
    }
}