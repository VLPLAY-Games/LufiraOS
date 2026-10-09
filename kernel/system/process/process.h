#pragma once

#include "lib/types.h"
#include "system/cpu/tss.h"
#include "fs/vfs/vfs.h"
#include "system/ipc/mailbox.h"
// lufira_ps_entry_t — для прототипа process_pslist() ниже. syscall.h не
// зависит от process.h, include однонаправленный, без цикла.
#include "system/syscall/syscall.h"

#define KERNEL_HEAP_START       0xFFFF900000000000ULL  // Куча ядра
#define KERNEL_STACK_AREA_START 0xFFFF880000000000ULL  // Область стеков
#define KERNEL_STACK_SIZE       (16 * 1024)            // 16KB на процесс
#define MAX_PROCESSES           32
#define USER_STACK_AREA_START 0x0000700000000000ULL

// Отдельный маленький стек для обработчиков сигналов (SYS_SIGACTION) — один
// и тот же виртуальный адрес у каждого процесса (не pid-сдвинутый, как
// USER_STACK_AREA_START) — своя PML4 на процесс, коллизий нет, ребёнку после
// fork() достаётся автоматически тем же постраничным копированием
// (clone_address_space_deep()). Нужен отдельный от основного user-стека:
// context.rsp на момент доставки сигнала может указывать на кернел-стек
// процесса (прерван внутри блокирующего syscall'а) — недоступен для
// ring3-обработчика (баг: обработчик падал в page fault на "своём" стеке).
#define SIGNAL_STACK_AREA_START 0x0000690000000000ULL
#define SIGNAL_STACK_SIZE (2 * PAGE_SIZE) // печать внутри обработчика (printf) — небольшой запас сверх 1 страницы
#define USER_STACK_SIZE       (16 * 1024)  // 16KB

// Область под mmap(). Один адрес для всех процессов (в отличие от
// USER_STACK_AREA_START, сдвигаемого на pid) — не нужно сдвигать, у каждого
// процесса своя таблица страниц. Индекс PML4 = 192, внутри диапазона 0..255
// (process_fork() копирует целиком), подальше от кода ELF и от
// USER_STACK_AREA_START (индекс 224).
#define MMAP_AREA_START   0x0000600000000000ULL
#define MAX_MMAP_REGIONS  32

// Бюджет под argv[]/envp[] на верху пользовательского стека (build_exec_stack())
// — содержимое строк + оба NULL-терминированных массива указателей.
// Оставляет USER_STACK_SIZE - MAX_EXEC_ARGS_BYTES = 12KB рабочего стека.
// MAX_EXEC_ARGS ограничивает count отдельно от байтового лимита — иначе
// много коротких строк переполнило бы сами массивы указателей раньше.
#define MAX_EXEC_ARGS        64
#define MAX_EXEC_ARGS_BYTES  4096

// Значение process_t.wait_target_pid, означающее "жду ЛЮБОГО своего
// ребёнка" (аналог waitpid(-1, ...)). 0 означает "не жду ничего" — реальные
// PID никогда не достигают этого значения.
#define WAIT_ANY_PID ((uint32_t)-1)

// Сигналы. SIGINT/SIGTERM ловятся (SYS_SIGACTION) — SIGKILL/SIGSTOP/SIGCONT
// только действие по умолчанию (POSIX: не ловятся, не игнорируются).
// Доставка отложенная: process_signal() выставляет pending_signal,
// process_deliver_pending_signal() (process.c) доставляет из
// syscall_handler() перед возвратом из каждого syscall'а — единственная
// точка, где указатель гарантированно настоящая ring3-точка возврата.
// Номера взяты как у POSIX-сигналов просто для привычности.
#define SIGINT  2
#define SIGALRM 14 // доставляется SYS_ALARM по истечении таймера
#define SIGKILL 9
#define SIGTERM 15
#define SIGCONT 18
#define SIGSTOP 19

// Максимальный номер сигнала + 1 — размер signal_handlers[] (process_t).
#define MAX_SIGNUM 32

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

// Один регион, выделенный sys_mmap(). length==0 — слот свободен. shared_id —
// индекс в реестре shm.c, -1 для MAP_PRIVATE-подобного региона. Копируется
// process_fork()'ом вместе с mmap_regions[] — ребёнок наследует тот же id,
// что и физически алиасированные страницы (clone_address_space_deep()).
typedef struct {
    uint64_t addr;
    uint64_t length;
    int shared_id;
} mmap_region_t;

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
    // Регионы sys_mmap()/sys_munmap() и bump-указатель на следующий
    // свободный адрес под MMAP_AREA_START — см. process_create()/
    // process_init() (инициализация), process_fork() (копируется вместе с
    // физически продублированными страницами) и process_commit_exec()
    // (сбрасывается — exec() заменяет всё адресное пространство целиком).
    mmap_region_t mmap_regions[MAX_MMAP_REGIONS];
    uint64_t next_mmap_addr;
    // Текущий рабочий каталог процесса (LufiraFS) — по умолчанию корень
    // (process_create()). fork() наследует 1:1 (POSIX), exec()
    // (process_commit_exec()) сознательно не трогает — execve() тоже
    // сохраняет cwd.
    char cwd_path[256];
    uint32_t cwd_inode;
    // Идентичность процесса (uid==0 — root, обходит все проверки прав).
    // Нет euid/egid — нет setuid-бита, "реальный" и "эффективный" всегда
    // совпадают. Новый процесс наследует uid/gid от current_process (в
    // отличие от cwd); fork() копирует 1:1, exec() не трогает. Меняется
    // только командой su (мутирует current_process->uid/gid напрямую, как
    // cd мутирует cwd_inode).
    uint32_t uid;
    uint32_t gid;
    // Помечает процесс, "исполняющий роль" шелла — остаётся истинным даже
    // после exec() (меняет образ на месте, PID не меняется). См.
    // is_shell-проверку в process_exit()/terminate_process_by_signal():
    // когда такой процесс завершается, пересоздаётся новый шелл — иначе
    // система осталась бы без интерактивного приглашения.
    int is_shell;
    // signal_handlers[sig] — адрес обработчика в user-space (0 — default).
    // pending_signal — недоставленный сигнал (один слот — сигналы не
    // настолько часты, чтобы нужна очередь; новый перезаписывает предыдущий).
    // in_signal_handler — внутри обработчика сейчас (новые сигналы не
    // доставляются, упрощённый аналог sigprocmask()). saved_signal_context —
    // снимок syscall_frame_t на момент редиректа в обработчик
    // (process_deliver_pending_signal()) — SYS_SIGRETURN переносит его
    // обратно (process_sigreturn()) и возобновляет исполнение точно с
    // места, где сигнал застал процесс.
    uint64_t signal_handlers[MAX_SIGNUM];
    int pending_signal;
    int in_signal_handler;
    process_context_t saved_signal_context;
    // Верхний адрес отдельного стека для обработчиков сигналов (0 — не
    // выделен, доставка пропускается, см. process_deliver_pending_signal()) —
    // см. SIGNAL_STACK_AREA_START выше.
    uint64_t sig_stack_top;
    // 0, если таймер не взведён, иначе тик PIT, на который взведён SIGALRM
    // (SYS_ALARM). Проверяется в той же точке, что пробуждение
    // PROCESS_SLEEPING (timer_irq_handler()), доставляется через обычный
    // process_signal().
    uint64_t alarm_tick;
    // 1 до первой активации процесса, затем 0. switch_to_process()
    // направляет самый первый запуск через context_enter_ring3() (настоящий
    // ring0->ring3 переход через iretq) вместо обычного context_switch()
    // (jmp, без смены CS/CPL) — иначе новый процесс исполнял бы первые
    // инструкции с привилегиями ядра. Все последующие переключения идут
    // через context_switch().
    int first_run;
    // Число тиков PIT (10мс каждый), когда этот процесс был current_process
    // при срабатывании timer_irq_handler() — оценка доли CPU для cpuload.
    // Не сбрасывается при exec(), начинается заново у ребёнка fork().
    uint64_t cpu_ticks;
    // Собственный почтовый ящик процесса (mailbox.h) — generic IPC, не
    // специфичный для GUI. Используется userspace WM-процессом (клиентские
    // RPC-запросы и input-события от ядра) и обычными клиентами (ответы WM).
    // process_create() заводит пустым; fork() ребёнку не копирует — свой,
    // отдельный от родителя (как и у POSIX fork() message queues не
    // наследуются).
    ipc_mailbox_t mailbox;
    // FXSAVE/FXRSTOR legacy area (x87/MMX/XMM0-15/MXCSR) — 512 bytes,
    // MUST be 16-byte aligned (FXSAVE/FXRSTOR #GP on an unaligned operand).
    // kmalloc() only guarantees 8-byte alignment (heap.c), so fpu_state is
    // a manually-aligned pointer INTO fpu_state_raw, not a raw kmalloc()
    // result — fpu_state_raw is what actually gets kfree()'d.
    // Allocated/initialized to a clean FPU state by alloc_fpu_state() in
    // process_create()/process_init(); fork() overwrites it with a live
    // fxsave() of the parent instead (process_fork()). See switch_to_process()
    // for where it's actually saved/restored on every context switch — the
    // kernel itself never touches XMM/FPU (-mgeneral-regs-only), but ring3
    // processes can (and for the planned DOOM port, will).
    uint8_t *fpu_state;
    void *fpu_state_raw;
    struct process *next;
} process_t;

void process_init(void);
process_t* process_create(const char *name, void (*entry)(void));

// Регистрирует spawner, который запускает НОВЫЙ процесс "shell", когда
// текущий is_shell-процесс завершится. Вызывается один раз из kernel.c
// после первого process_create("shell", ...). Шелл — настоящий userspace
// ELF (/bin/shell.elf): spawner возвращает уже полностью подготовленный
// (process_create + ELF загружен + стек/argv собраны) process_t*, is_shell
// выставляет сам. NULL при ошибке — respawn_shell_if_needed() тогда просто
// не восстанавливает шелл.
void process_set_shell_spawner(process_t *(*spawner)(void));
void process_exit(int exit_code);
void schedule(void);
void switch_to_process(process_t *next);
void process_reap(void);
void process_sleep(uint64_t milliseconds);
void process_ps(void);
int process_kill(uint32_t pid);

// SYS_PSLIST (30): та же прогулка по process_list под irq_disable(), что
// делает process_ps() — только пишет в out[] вместо printf(). out — уже
// провалидированный указатель пользовательского буфера (is_user_range_valid()
// — забота sys_pslist()); ядро пишет напрямую под CR3 вызывающего. Возвращает
// число записанных записей (0..max_count).
int process_pslist(lufira_ps_entry_t *out, uint32_t max_count);

// Отправляет сигнал sig процессу pid и сразу применяет его действие по
// умолчанию (см. SIGKILL/SIGTERM/SIGSTOP/SIGCONT выше): SIGKILL/SIGTERM
// завершают процесс (exit_code = 128+sig, как в реальных шеллах),
// SIGSTOP/SIGCONT останавливают/возобновляют его. process_kill() — просто
// process_signal(pid, SIGKILL). Возвращает 0 при успехе, -1 если процесс
// не найден (или сигнал неизвестен).
int process_signal(uint32_t pid, int sig);

// SYS_SET_FOREGROUND (31): даёт userspace выставлять/снимать foreground_pid
// — то же поле, которым пользуется shell_handle_ctrl_c() для Ctrl+C.
// target_pid==0 снимает (всегда разрешено); иначе требует, чтобы target_pid
// был любым потомком caller_pid, не только прямым ребёнком — поднимается по
// цепочке ppid вверх, пока не найдёт caller_pid или корень дерева. 0 при
// успехе, -1 если target_pid не найден или не потомок.
int process_set_foreground(uint32_t caller_pid, uint32_t target_pid);

// Текущий foreground_pid (0, если не выставлен) — чтобы процесс мог
// проверить, что он сам прямо сейчас foreground, не передавая фокус заново.
uint32_t process_get_foreground(void);

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

// Строит argv[]/envp[] (содержимое строк + оба NULL-терминированных массива
// указателей) на верху пользовательского стека new_pml4, растущего вниз от
// stack_top. argv/envp читаются из готовых kernel-side массивов (вызывающий
// отвечает за их валидность/освобождение). *rsp_out получает начальный RSP,
// *argv_out/*envp_out — виртуальные адреса построенных массивов (в rsi/rdx
// контекста, см. elf.c). Возвращает 0 при успехе, -1 если не умещаются в
// MAX_EXEC_ARGS/MAX_EXEC_ARGS_BYTES.
int build_exec_stack(uint64_t new_pml4, uint64_t stack_top,
                      char *const argv[], char *const envp[],
                      uint64_t *rsp_out, uint64_t *argv_out, uint64_t *envp_out);

// Настоящий fork(): дублирует текущий процесс (адресное пространство —
// eager copy, fd-таблица — общие file_t/inode с увеличенным ref_count).
// frame_ptr — указатель на кадр регистров, сохранённый syscall_entry.S
// (нужен, чтобы ребёнок продолжил выполнение с той же инструкции user-кода,
// что и родитель). Возвращает pid ребёнка (родителю) или (uint64_t)-1.
uint64_t process_fork(uint64_t frame_ptr);
// См. определение (process.c) и SYS_SIGRETURN (syscall.h).
uint64_t process_sigreturn(uint64_t frame_ptr);
// Зовётся из syscall_handler() перед возвратом из каждого syscall'а — см.
// определение (process.c).
void process_deliver_pending_signal(uint64_t frame_ptr);

extern uint64_t kernel_cr3;

extern void context_switch(process_context_t *old_context,
                          process_context_t *new_context);
extern void context_enter_ring3(process_context_t *old_context,
                          process_context_t *new_context);

extern process_t *current_process;
extern process_t *process_list;

// 1, если current_process — служебный idle-процесс (crn3-цикл hlt, pid=0),
// т.е. в данный момент реально никакая другая работа не выполняется.
// Используется timer_irq_handler() (pit.c) для подсчёта простоя под cpuload.
int process_is_idle(void);

// PID процесса, "на переднем плане" (запущен через run/exec из шелла) —
// цель для Ctrl+C. 0 = нет такого (runbg сюда не попадают). Выставляется в
// elf.c при запуске, сбрасывается в process.c при завершении (см.
// terminate_process_by_signal()/process_exit()).
extern volatile uint32_t foreground_pid;

// Ищет process_t по pid в process_list. NULL, если не найден. Экспортирована
// (была static в process.c), т.к. нужна и mailbox_send() (mailbox.c) для
// поиска адресата по pid любого, не только родственного, процесса.
process_t* process_find_by_pid(uint32_t pid);

// PID зарегистрированного оконного сервера (SYS_WM_REGISTER) — 0, если никто
// не зарегистрирован (SYS_WIN_* отказывают). WM — обычный userspace-процесс
// (lufira-packages/src/apps/wm.c), единственная связь с ядром — этот pid
// (куда syscall.c релеит SYS_WIN_* как RPC) и привилегированные
// SYS_FB_*/SYS_WM_REGISTER. Не сбрасывается явно, только когда сам WM
// завершается (process_exit() обнуляет его, как и foreground_pid).
extern volatile uint32_t g_wm_pid;

// 0 при успехе, -1 если WM уже зарегистрирован (ровно один на систему).
int process_wm_register(uint32_t pid);
uint32_t process_get_wm_pid(void);

// 1, если следующий вызов shell_task() — пересоздание после того, как
// exec() подменил процесс "shell", а подменившая программа завершилась/
// была убита (см. respawn_shell_if_needed(), process.c). shell_task()
// читает и сбрасывает флаг сам, чтобы не печатать заново баннер/"Type help".
extern volatile int shell_is_respawn;