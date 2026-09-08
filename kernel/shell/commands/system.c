#include "lib/string.h"
#include "lib/cpu.h"
#include "../commands.h"
#include "drivers/console/console.h"
#include "../shell.h"
#include "fs/fat/fat.h"
#include "system/acpi/acpi.h"
#include "system/process/process.h"
#include "system/mm/heap.h"
#include "system/elf/elf.h"

extern fat_fs_t fatfs;

// help, clear, reboot, shutdown, version, status, trap
void command_help(void) {
    printf("\nAvailable commands:\n");
    printf(" help - Show this help\n");
    printf(" clear - Clear screen\n");
    printf(" reboot - Reboot system\n");
    printf(" shutdown - Shutdown system\n");
    printf(" version - Show kernel version\n");
    printf(" echo - Echo text back\n");
    printf(" history - Show command history\n");
    printf(" status - Show interrupt/CPU status\n");
    printf(" trap - Trigger test exceptions\n");
    printf(" color - Set console colors or reset\n");
    printf(" colors - Show available colors\n");
    printf(" fg <color> - Set foreground color\n");
    printf(" bg <color> - Set background color\n");
    printf("\nFile system commands:\n");
    printf(" pwd - Print current directory\n");
    printf(" cd <dir> - Change directory\n");
    printf(" ls [-l] - List directory contents\n");
    printf(" mkdir <name> - Create directory\n");
    printf(" rm <name> - Remove file/directory\n");
    printf(" cp <src> <dst> - Copy file\n");
    printf(" mv <src> <dst> - Move/rename file\n");
    printf(" cat <file> - Display file content\n");
    printf(" touch <filename> - Create empty file\n");
    printf(" write <file> <text> - Write text to file\n");
    printf(" edit <file> <text> - Append text to file\n");
    printf(" run <file> - Execute ELF program\n");
    printf(" runbg <file> - Execute ELF program in background\n");
    printf(" kill [-SIGNAL] <pid> - Send a signal (-TERM/-KILL/-STOP/-CONT, default -TERM)\n");
    printf(" wait <pid> - Wait for a child process to exit\n");
    printf(" beep - Play beep to check sound\n");
    printf(" music - Play sample music to check sound\n");
    printf(" mixer <volume> - Change sound volume\n");
    printf(" exec <file> - Replace current process with ELF program\n");
}

void command_clear(void) { clear_screen(); show_prompt(); }
void command_reboot(void) {
    printf("\nSyncing filesystem... ");
    fat_flush(&fatfs);
    printf("done.\nRebooting system...\n");
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    __asm__ volatile ("outw %0, %1" : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    printf("Reboot failed. Please restart manually.\n");
}
void command_shutdown(void) {
    printf("\nSyncing filesystem... ");
    fat_flush(&fatfs);
    printf("done.\nShutting down system...\n");
    
    // Используем ACPI shutdown
    acpi_shutdown();
    
    // Если ACPI не сработал, пробуем legacy
    printf("Shutdown command sent. System may require manual power off.\n");
}
void command_version(void) {
    printf("\nLufiraOS Kernel v0.1.0\nBuilt: %s %s\nArchitecture: x86_64\n", __DATE__, __TIME__);
}
void command_status(void) {
    printf("\nSYSTEM STATUS:\n");
    printf("--------------\n");
    printf(" Interrupt Flag: %s\n", interrupts_enabled() ? "SET" : "CLEAR");
    printf(" Interrupts: %s\n", interrupts_enabled() ? "ENABLED" : "DISABLED");
    printf(" CPU Test: trap int3 / ud2 / pf\n");
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

void command_runbg(const char *filename) {
    if (!filename || *filename == '\0') {
        printf("\nUsage: runbg <filename>\n");
        printf("Example: runbg hello.elf\n");
        return;
    }

    uint32_t fsize;
    if (fat_open(&fatfs, filename, &fsize) != 0) {
        printf("\nFile not found: %s\n", filename);
        return;
    }

    uint8_t *file_buf = (uint8_t *)kmalloc(fsize);
    if (!file_buf) {
        printf("\nNot enough memory to load %s (%u bytes)\n",
               filename,
               fsize);
        return;
    }

    int br = fat_read_file(&fatfs, filename, file_buf, fsize);
    if (br <= 0) {
        printf("\nError reading file: %s\n", filename);
        kfree(file_buf);
        return;
    }

    printf("\nLoading ELF in background: %s (%u bytes)...\n",
           filename,
           fsize);

    int pid = elf_exec_background(file_buf, fsize, filename);
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

        if (token_equals(sig_arg, "KILL")) sig = SIGKILL;
        else if (token_equals(sig_arg, "TERM")) sig = SIGTERM;
        else if (token_equals(sig_arg, "STOP")) sig = SIGSTOP;
        else if (token_equals(sig_arg, "CONT")) sig = SIGCONT;
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