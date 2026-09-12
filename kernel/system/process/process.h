#pragma once

#include "lib/types.h"
#include "system/cpu/tss.h"
#include "fs/vfs/vfs.h"

#define KERNEL_HEAP_START       0xFFFF900000000000ULL  // Куча ядра
#define KERNEL_STACK_AREA_START 0xFFFF880000000000ULL  // Область стеков
#define KERNEL_STACK_SIZE       (16 * 1024)            // 16KB на процесс
#define MAX_PROCESSES           32
#define USER_STACK_AREA_START 0x0000700000000000ULL
#define USER_STACK_SIZE       (16 * 1024)  // 16KB

// Значение process_t.wait_target_pid, означающее "жду ЛЮБОГО своего
// ребёнка" (аналог waitpid(-1, ...)). 0 означает "не жду ничего" — реальные
// PID никогда не достигают этого значения.
#define WAIT_ANY_PID ((uint32_t)-1)

// Сигналы: только действия по умолчанию (нет sigaction()/обработчиков в
// user-space) — доставка синхронная, прямо в момент отправки (см.
// process_signal() в process.c). Номера взяты как у настоящих POSIX-сигналов
// просто для привычности.
#define SIGINT  2
#define SIGKILL 9
#define SIGTERM 15
#define SIGCONT 18
#define SIGSTOP 19

typedef enum {
    PROCESS_READY = 0,
    PROCESS_RUNNING = 1,
    PROCESS_BLOCKED = 2,
    PROCESS_SLEEPING = 3,
    PROCESS_TERMINATED = 4,
    PROCESS_STOPPED = 5     // остановлен SIGSTOP, ждёт SIGCONT
} process_state_t;

typedef struct __attribute__((packed)) {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rsp;
    uint64_t rip;
    uint64_t rflags;
    uint64_t cr3;
} process_context_t;

typedef struct process {
    uint32_t pid;
    uint32_t ppid;          // 0 = нет родителя (например, до fork()/wait())
    char name[32];
    process_state_t state;
    uint64_t wakeup_tick;
    int exit_code;          // валиден, когда state == PROCESS_TERMINATED
    uint32_t wait_target_pid; // 0 = не жду; WAIT_ANY_PID = жду любого ребёнка; иначе конкретный pid
    process_context_t context;
    uint64_t stack_base;
    uint64_t stack_size;
    uint64_t ring0_stack;
    uint64_t ring0_stack_pages;
    uint64_t page_table;
    fd_table_t fd_table;    // собственная таблица дескрипторов процесса
    // Помечает процесс, который в данный момент "исполняет роль" шелла —
    // изначально сам шелл, и остаётся истинным даже после exec() (тот
    // меняет образ процесса НА МЕСТЕ, PID/process_t не меняются). См.
    // process_set_shell_entry()/is_shell-проверку в process_exit() и
    // terminate_process_by_signal(): когда такой процесс завершается,
    // оригинального шелла для возврата уже не существует (exec не форкает),
    // так что вместо него пересоздаётся новый — иначе система осталась бы
    // вообще без интерактивного приглашения.
    int is_shell;
    struct process *next;
} process_t;

void process_init(void);
process_t* process_create(const char *name, void (*entry)(void));

// Регистрирует entry-функцию, которую нужно запустить как НОВЫЙ процесс
// "shell", когда текущий is_shell-процесс завершится (см. is_shell в
// process_t выше). Вызывается один раз из kernel.c сразу после самого
// первого process_create("shell", ...).
void process_set_shell_entry(void (*entry)(void));
void process_exit(int exit_code);
void schedule(void);
void switch_to_process(process_t *next);
void process_reap(void);
void process_sleep(uint64_t milliseconds);
void process_ps(void);
int process_kill(uint32_t pid);

// Отправляет сигнал sig процессу pid и сразу применяет его действие по
// умолчанию (см. SIGKILL/SIGTERM/SIGSTOP/SIGCONT выше): SIGKILL/SIGTERM
// завершают процесс (exit_code = 128+sig, как в реальных шеллах),
// SIGSTOP/SIGCONT останавливают/возобновляют его. process_kill() — просто
// process_signal(pid, SIGKILL). Возвращает 0 при успехе, -1 если процесс
// не найден (или сигнал неизвестен).
int process_signal(uint32_t pid, int sig);

// Немедленно убирает proc из списка планировщика (используется только для
// отката недостроенного процесса, например если fork() не смог
// скопировать адресное пространство ребёнка).
void process_discard(process_t *proc);

// Ждёт завершения ребёнка текущего процесса: pid == 0 значит "любой
// ребёнок" (внутри хранится как WAIT_ANY_PID), иначе конкретный pid.
// Блокирует вызывающего, если такой ребёнок жив, реап'ит зомби и
// возвращает его pid + *status_out = exit_code. Возвращает -1, если у
// вызывающего вообще нет такого ребёнка (в том числе уже отреапленного).
int process_wait(uint32_t pid, int *status_out);

// Готовит новое адресное пространство и пользовательский стек для exec(),
// не трогая текущий образ proc (см. elf_exec_replace()).
int process_prepare_exec(
    process_t *proc,
    uint64_t *new_pml4_out,
    uint64_t *new_stack_out
);

// Подтверждает exec(): переключает proc на уже подготовленные (и
// заполненные) адресное пространство и стек, оставляя тот же PID.
int process_commit_exec(
    process_t *proc,
    uint64_t new_pml4,
    uint64_t new_stack,
    const char *name
);

// Настоящий fork(): дублирует текущий процесс (адресное пространство —
// eager copy, fd-таблица — общие file_t/inode с увеличенным ref_count).
// frame_ptr — указатель на кадр регистров, сохранённый syscall_entry.S
// (нужен, чтобы ребёнок продолжил выполнение с той же инструкции user-кода,
// что и родитель). Возвращает pid ребёнка (родителю) или (uint64_t)-1.
uint64_t process_fork(uint64_t frame_ptr);

extern uint64_t kernel_cr3;

extern void context_switch(process_context_t *old_context, 
                          process_context_t *new_context);

extern process_t *current_process;
extern process_t *process_list;

// PID процесса, который сейчас "на переднем плане" (запущен через run/exec
// из шелла) — цель для Ctrl+C. 0 = нет такого (обычные background-процессы
// runbg сюда никогда не попадают). Выставляется в elf.c при запуске,
// сбрасывается в process.c при завершении процесса (нормальном или по
// сигналу) — см. подробности у terminate_process_by_signal()/process_exit().
extern volatile uint32_t foreground_pid;