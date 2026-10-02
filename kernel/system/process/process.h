#pragma once

#include "lib/types.h"
#include "system/cpu/tss.h"
#include "fs/vfs/vfs.h"
// lufira_ps_entry_t — для прототипа process_pslist() ниже (v0.7 план,
// этап 5, под-этап 4). syscall.h не зависит от process.h, так что этот
// include однонаправленный, без цикла.
#include "system/syscall/syscall.h"

#define KERNEL_HEAP_START       0xFFFF900000000000ULL  // Куча ядра
#define KERNEL_STACK_AREA_START 0xFFFF880000000000ULL  // Область стеков
#define KERNEL_STACK_SIZE       (16 * 1024)            // 16KB на процесс
#define MAX_PROCESSES           32
#define USER_STACK_AREA_START 0x0000700000000000ULL

// Отдельный маленький стек для обработчиков сигналов (v0.8-мост, пункт 5,
// SYS_SIGACTION) — ОДИН и тот же виртуальный адрес у каждого процесса (не
// pid-сдвинутый, как USER_STACK_AREA_START выше): у каждого процесса своя
// собственная PML4, так что коллизии между процессами тут в принципе нет,
// а ребёнку после fork() этот адрес достаётся автоматически, тем же общим
// постраничным копированием, что и весь остальной адрес процесса
// (clone_address_space_deep(), process.c) — которое просто копирует уже
// СУЩЕСТВУЮЩИЕ маппинги по их текущим виртуальным адресам, а не
// пересчитывает их заново под pid ребёнка. Нужен отдельный от основного
// user-стека, а не просто кусок того же: context.rsp на момент доставки
// сигнала может указывать на КЕРНЕЛ-стек процесса (если его прервали
// глубоко внутри блокирующего syscall'а, напр. process_sleep()) — и этот
// адрес для ring3-обработчика попросту недоступен (найдено живым
// тестированием: обработчик падал в page fault на первой же попытке
// использовать "свой" стек).
#define SIGNAL_STACK_AREA_START 0x0000690000000000ULL
#define SIGNAL_STACK_SIZE (2 * PAGE_SIZE) // печать внутри обработчика (printf) — небольшой запас сверх 1 страницы
#define USER_STACK_SIZE       (16 * 1024)  // 16KB

// Область под mmap(). Один и тот же виртуальный адрес для ВСЕХ процессов
// (в отличие от USER_STACK_AREA_START, который сдвигается на pid) — не
// нужно: у каждого процесса своя приватная таблица страниц, так что
// виртуальные адреса разных процессов никогда физически не пересекаются.
// Индекс PML4 = 0x600000000000 / 2^39 = 192 — внутри диапазона 0..255,
// который process_fork()/clone_address_space_deep() уже копирует целиком,
// и подальше от кода ELF (0x400000+) и от USER_STACK_AREA_START (индекс 224).
#define MMAP_AREA_START   0x0000600000000000ULL
#define MAX_MMAP_REGIONS  32

// Бюджет под argv[]/envp[] на верху пользовательского стека нового
// процесса (build_exec_stack() в process.c) — само содержимое строк +
// оба NULL-терминированных массива указателей. Оставляет
// USER_STACK_SIZE - MAX_EXEC_ARGS_BYTES = 12KB настоящего рабочего стека.
// MAX_EXEC_ARGS ограничивает count независимо от суммарной длины строк —
// иначе много очень коротких строк переполнило бы сами массивы указателей
// раньше, чем сработает байтовый лимит.
#define MAX_EXEC_ARGS        64
#define MAX_EXEC_ARGS_BYTES  4096

// Значение process_t.wait_target_pid, означающее "жду ЛЮБОГО своего
// ребёнка" (аналог waitpid(-1, ...)). 0 означает "не жду ничего" — реальные
// PID никогда не достигают этого значения.
#define WAIT_ANY_PID ((uint32_t)-1)

// Сигналы. v0.8-мост, пункт 5: SIGINT/SIGTERM теперь ловятся (SYS_SIGACTION,
// syscall.h) — SIGKILL/SIGSTOP/SIGCONT по-прежнему только действие по
// умолчанию (как и в настоящем POSIX — их нельзя ни поймать, ни
// проигнорировать). Доставка — ОТЛОЖЕННАЯ, не синхронная: process_signal()
// только выставляет pending_signal, а реальный переход на обработчик —
// process_deliver_pending_signal() (process.c), вызывается из
// syscall_handler() (syscall.c) прямо перед возвратом ИЗ КАЖДОГО
// syscall'а — единственная точка, где доступный указатель (syscall_frame_t*)
// гарантированно настоящая ring3-точка возврата, независимо от того, кто и
// когда вызвал process_signal() (даже сам процесс в себя, из прерывания
// клавиатуры прямо во время своего же исполнения) — см. подробный
// комментарий там же. Номера взяты как у настоящих POSIX-сигналов просто
// для привычности.
#define SIGINT  2
#define SIGALRM 14 // v0.8-мост, пункт 6 — доставляется SYS_ALARM по истечении таймера
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

// Один регион, выделенный sys_mmap(). length==0 — слот свободен.
// shared_id — индекс в реестре shm.c (v0.8-мост, пункт 4), -1 для
// обычного (MAP_PRIVATE-подобного) региона. Копируется process_fork()'ом
// вместе со всем остальным mmap_regions[] — ребёнок наследует тот же id,
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
    // Текущий рабочий каталог процесса (LufiraFS) — раньше жил ТОЛЬКО в
    // шелл-глобалах (kernel/shell/shell.c), что было неверно: шелл — просто
    // один из процессов. По умолчанию корень (см. process_create()).
    // fork() наследует 1:1 (POSIX), exec() (process_commit_exec())
    // сознательно НЕ трогает — execve() тоже сохраняет cwd.
    char cwd_path[256];
    uint32_t cwd_inode;
    // Идентичность процесса (uid==0 — root, обходит все проверки прав в
    // LufiraFS). Нет euid/egid — в этой минимальной реализации нет setuid-
    // бита, так что "реальный" и "эффективный" всегда совпадают. Новый
    // процесс наследует uid/gid от current_process (process_create()) —
    // в отличие от cwd, который всегда сбрасывается на корень; fork()
    // копирует 1:1 (POSIX), exec() (process_commit_exec()) сознательно НЕ
    // трогает (execve() тоже сохраняет identity без setuid-бита). Меняется
    // только командой su (шелл мутирует current_process->uid/gid напрямую,
    // как cd мутирует cwd_inode — без отдельного syscall).
    uint32_t uid;
    uint32_t gid;
    // Помечает процесс, который в данный момент "исполняет роль" шелла —
    // изначально сам шелл, и остаётся истинным даже после exec() (тот
    // меняет образ процесса НА МЕСТЕ, PID/process_t не меняются). См.
    // process_set_shell_entry()/is_shell-проверку в process_exit() и
    // terminate_process_by_signal(): когда такой процесс завершается,
    // оригинального шелла для возврата уже не существует (exec не форкает),
    // так что вместо него пересоздаётся новый — иначе система осталась бы
    // вообще без интерактивного приглашения.
    int is_shell;
    // v0.8-мост, пункт 5 (SYS_SIGACTION/SYS_SIGRETURN, см. комментарий у
    // SIGINT выше): signal_handlers[sig] — адрес обработчика в user-space
    // (0 = действие по умолчанию). pending_signal — сигнал, ещё не
    // доставленный (один слот — тот же принцип минимализма, что и у
    // console_input_waiter/pipe_t.read_waiter: на практике сигналы здесь
    // не настолько часты, чтобы нужна была очередь; новый ПЕРЕЗАПИСЫВАЕТ
    // предыдущий недоставленный, как и везде в этом ядре). in_signal_handler
    // — внутри обработчика сейчас или нет (пока 1 — новые сигналы не
    // доставляются, тот же дух, что sigprocmask() во время настоящего
    // обработчика в POSIX, упрощённо: всё, а не только сам этот сигнал).
    // saved_signal_context — полный снимок syscall_frame_t на момент
    // редиректа в обработчик (process_deliver_pending_signal(), process.c)
    // — SYS_SIGRETURN переносит его обратно в syscall_frame_t вызывающего
    // (process_sigreturn(), process.c) и возобновляет исполнение ТОЧНО с
    // того места, где сигнал застал процесс.
    uint64_t signal_handlers[MAX_SIGNUM];
    int pending_signal;
    int in_signal_handler;
    process_context_t saved_signal_context;
    // Верхний адрес отдельного стека для обработчиков сигналов (0, если
    // ещё не выделен/недоступен — тогда доставка сигнала просто
    // пропускается, см. process_deliver_pending_signal()) — см.
    // SIGNAL_STACK_AREA_START выше.
    uint64_t sig_stack_top;
    // v0.8-мост, пункт 6 (SYS_ALARM) — 0, если таймер не взведён, иначе
    // тик PIT (см. pit_get_ticks()), на который взведён SIGALRM. Проверяется
    // в той же точке, что и пробуждение PROCESS_SLEEPING (timer_irq_handler(),
    // pit.c), и доставляется через тот же process_signal(), что и обычный
    // kill() — ничего отдельного изобретать не пришлось.
    uint64_t alarm_tick;
    // 1 до первой активации процесса, затем всегда 0. switch_to_process()
    // использует это, чтобы направить САМЫЙ первый запуск через
    // context_enter_ring3() (настоящий ring0->ring3 переход через iretq)
    // вместо обычного context_switch() (jmp, без смены CS/CPL) — иначе
    // новый процесс выполнял бы свои первые инструкции с привилегиями
    // ядра вплоть до первого возврата из syscall. Все последующие
    // переключения (в т.ч. процесса, вытесненного планировщиком прямо из
    // ring3) всегда идут через context_switch() — это уже не "холодный
    // старт", а возобновление прерванного вызова.
    int first_run;
    // Число тиков PIT (10мс каждый), в течение которых этот процесс был
    // current_process при срабатывании timer_irq_handler() — грубая, но
    // достаточная для команды cpuload оценка доли CPU (см. pit.c). Не
    // сбрасывается при exec() (реальный ps/top тоже считают CPU-время
    // накопительно по PID через exec), но начинается заново у ребёнка
    // fork() (см. process_fork()) — он ещё не выполнялся.
    uint64_t cpu_ticks;
    struct process *next;
} process_t;

void process_init(void);
process_t* process_create(const char *name, void (*entry)(void));

// Регистрирует entry-функцию, которую нужно запустить как НОВЫЙ процесс
// "shell", когда текущий is_shell-процесс завершится (см. is_shell в
// process_t выше). Вызывается один раз из kernel.c сразу после самого
// первого process_create("shell", ...).
// v0.7 план, этап 5, под-этап 6: шелл теперь настоящий userspace ELF
// (/bin/shell.elf), а не кернел-функция — регистрируемый callback
// возвращает уже полностью подготовленный (process_create + ELF загружен +
// стек/argv собраны) process_t*, а не просто указатель на функцию для
// process_create(name, entry). is_shell выставляет сам spawner. NULL при
// ошибке (файл не найден/не загрузился) — respawn_shell_if_needed() тогда
// просто не восстанавливает шелл (система остаётся без интерактивного
// приглашения, что лучше отражает реальную проблему, чем тихий откат на
// старый kernel-native путь, которого больше нет).
void process_set_shell_spawner(process_t *(*spawner)(void));
void process_exit(int exit_code);
void schedule(void);
void switch_to_process(process_t *next);
void process_reap(void);
void process_sleep(uint64_t milliseconds);
void process_ps(void);
int process_kill(uint32_t pid);

// SYS_PSLIST (30, v0.7 план этап 5 под-этап 4): та же прогулка по
// process_list под irq_disable()/irq_enable(), что уже делают process_ps()
// и command_cpuload() (system.c) внутри ядра — только пишет в out[] вместо
// printf(). out — уже провалидированный указатель ПОЛЬЗОВАТЕЛЬСКОГО буфера
// (проверка is_user_range_valid() — забота sys_pslist(), syscall.c; ядро
// пишет туда напрямую под CR3 вызывающего процесса, тот же приём, что уже
// у sys_readdir()/sys_statfs()). Возвращает число реально записанных
// записей (0..max_count); живых процессов больше max_count в этой ОС быть
// не может (MAX_PROCESSES выше — общий потолок process_create()).
int process_pslist(lufira_ps_entry_t *out, uint32_t max_count);

// Отправляет сигнал sig процессу pid и сразу применяет его действие по
// умолчанию (см. SIGKILL/SIGTERM/SIGSTOP/SIGCONT выше): SIGKILL/SIGTERM
// завершают процесс (exit_code = 128+sig, как в реальных шеллах),
// SIGSTOP/SIGCONT останавливают/возобновляют его. process_kill() — просто
// process_signal(pid, SIGKILL). Возвращает 0 при успехе, -1 если процесс
// не найден (или сигнал неизвестен).
int process_signal(uint32_t pid, int sig);

// SYS_SET_FOREGROUND (31, v0.7 план этап 5 под-этап 6): даёт userspace
// (будущему shell.elf) выставлять/снимать foreground_pid — то же поле,
// которым уже сегодня пользуется kernel-native command_run() и
// shell_handle_ctrl_c() (shell.c) для Ctrl+C. target_pid == 0 снимает
// (всегда разрешено); иначе требует, чтобы target_pid был РЕАЛЬНЫМ
// ребёнком caller_pid (p->ppid == caller_pid) — процесс не может назначить
// foreground чужого, не своего процесса. Возвращает 0 при успехе, -1 если
// target_pid не найден или не свой.
int process_set_foreground(uint32_t caller_pid, uint32_t target_pid);

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

// Строит argv[]/envp[] (содержимое строк + оба NULL-терминированных
// массива указателей на них) на верху пользовательского стека нового
// адресного пространства new_pml4, растущего вниз от stack_top (обычно —
// stack_base только что созданного/подготовленного для exec() процесса).
// argv/envp сами читаются из уже готовых, NUL-терминированных
// kernel-side массивов (вызывающий отвечает за их валидность и
// освобождение — эта функция их не трогает и не сохраняет). argc/envc
// вычисляются здесь же сканированием на NULL, до MAX_EXEC_ARGS.
// *rsp_out получает готовый начальный RSP (ниже всех построенных данных —
// именно оттуда должен расти реальный стек main()), *argv_out/*envp_out —
// виртуальные адреса построенных массивов указателей (кладутся в
// rsi/rdx контекста, см. вызывающих в elf.c). Возвращает 0 при успехе,
// -1 если argv/envp не умещаются в MAX_EXEC_ARGS/MAX_EXEC_ARGS_BYTES.
int build_exec_stack(uint64_t new_pml4, uint64_t stack_top,
                      char *const argv[], char *const envp[],
                      uint64_t *rsp_out, uint64_t *argv_out, uint64_t *envp_out);

// Настоящий fork(): дублирует текущий процесс (адресное пространство —
// eager copy, fd-таблица — общие file_t/inode с увеличенным ref_count).
// frame_ptr — указатель на кадр регистров, сохранённый syscall_entry.S
// (нужен, чтобы ребёнок продолжил выполнение с той же инструкции user-кода,
// что и родитель). Возвращает pid ребёнка (родителю) или (uint64_t)-1.
uint64_t process_fork(uint64_t frame_ptr);
// v0.8-мост, пункт 5 — см. подробный комментарий у его определения
// (process.c) и у SYS_SIGRETURN (syscall.h).
uint64_t process_sigreturn(uint64_t frame_ptr);
// Зовётся из syscall_handler() (syscall.c) прямо перед возвратом ИЗ
// КАЖДОГО syscall'а — см. подробный комментарий у определения (process.c).
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

// PID процесса, который сейчас "на переднем плане" (запущен через run/exec
// из шелла) — цель для Ctrl+C. 0 = нет такого (обычные background-процессы
// runbg сюда никогда не попадают). Выставляется в elf.c при запуске,
// сбрасывается в process.c при завершении процесса (нормальном или по
// сигналу) — см. подробности у terminate_process_by_signal()/process_exit().
extern volatile uint32_t foreground_pid;

// 1, если следующий вызов shell_task() (kernel.c) — это пересоздание после
// того, как реальный exec (elf_exec_replace()) подменил собой процесс
// "shell", а подменившая его программа затем завершилась/была убита (см.
// respawn_shell_if_needed() в process.c). shell_task() читает и сбрасывает
// этот флаг сам, чтобы в таком случае не печатать заново вступительный
// баннер/"Type help" — иначе пересоздание процесса было слишком заметно
// выглядело как настоящая перезагрузка системы, хотя это просто новый
// экземпляр того же самого шелла.
extern volatile int shell_is_respawn;