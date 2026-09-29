#pragma once

#include "lib/types.h"

// Номера системных вызовов
#define SYS_WRITE    0
#define SYS_READ     1
#define SYS_EXIT     2
#define SYS_GETPID   3
#define SYS_YIELD    4
#define SYS_GETTICK  5
#define SYS_OPEN     6
#define SYS_CLOSE    7
#define SYS_SEEK     8
#define SYS_MMAP     9
#define SYS_MUNMAP   10
#define SYS_EXEC     11
#define SYS_FORK     12
#define SYS_WAIT     13
#define SYS_GETCWD   14
#define SYS_CHDIR    15
#define SYS_SLEEP    16
#define SYS_KILL     17
#define SYS_PIPE     18
#define SYS_CHMOD    19
#define SYS_CHOWN    20
#define SYS_GETUID   21
#define SYS_GETGID   22
#define SYS_MKDIR    23
#define SYS_RMDIR    24
#define SYS_UNLINK   25
#define SYS_READDIR  26
#define SYS_STATFS   27
#define SYS_MEMINFO  28
#define SYS_CPULOAD  29
#define SYS_PSLIST   30
// SYS_SET_FOREGROUND (31): pid (0 — снять). v0.7 план, этап 5, под-этап 6
// (Ctrl+C). foreground_pid (process.h) уже существует и уже безопасно
// используется shell_handle_ctrl_c() (kernel/shell/shell.c, отложенно из
// timer_irq_handler() — см. комментарий у shell_ctrl_c_pending в shell.h)
// для process_signal(foreground_pid, SIGINT) — сегодня его выставляет
// только кернел-нативный command_run(). Этот syscall даёт то же самое
// userspace-процессу (будущему shell.elf): fork() → SYS_SET_FOREGROUND(pid
// ребёнка) → SYS_WAIT(pid) — если Ctrl+C убьёт ребёнка, SYS_WAIT вернётся
// как обычно (тот же путь, что и при естественном завершении), никакого
// отдельного уведомления не нужно. Разрешено выставлять только PID
// СОБСТВЕННОГО ребёнка (process_set_foreground() проверяет ppid) — не
// чужой процесс.
#define SYS_SET_FOREGROUND 31

// Флаги для sys_open
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     4
#define O_TRUNC     8
#define O_APPEND    16

// Флаги для sys_seek
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

// Флаги для sys_mmap
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

// Флаги flags для sys_mmap (значения как в Linux — незачем изобретать свои,
// пригодится для совместимости с будущей libc). sys_mmap требует
// MAP_ANONYMOUS (файловый mmap не поддерживается) и отвергает MAP_FIXED
// (свой адрес вызывающего в этой версии не учитывается вообще — молча
// игнорировать было бы хуже отказа, вызывающий решил бы, что адрес учли).
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20

// Коды ошибок — подмножество POSIX/Linux errno (те же числа, незачем
// изобретать свои — пригодится будущей libc). Возвращаются из syscall'ов
// как (uint64_t)-CODE, тем же соглашением, что уже использовалось для
// (uint64_t)-1 везде в этом файле. Только то, что реально различают новые
// проверки user-указателей и sys_getcwd()/sys_chdir() — остальные
// (VFS-уровня) сбои пока остаются простым -1, см. syscall.c.
#define EPERM    1
#define ENOENT   2
#define EACCES   13
#define EFAULT   14
#define ENOTDIR  20
#define EINVAL   22
#define ERANGE   34

// Структуры для SYS_STATFS/SYS_MEMINFO/SYS_CPULOAD (v0.7, этап 1) — ровно
// те же данные, что уже печатают kernel-native command_df()/command_free()/
// command_cpuload() (kernel/shell/commands/{filesystem,system}.c) напрямую
// из lufirafs.sb/pmm_get_*()/heap_get_stats()/pit_get_*(), только через
// буфер в пользовательском пространстве вместо printf() внутри ядра —
// первый шаг выноса du/df/free/cpuload в отдельные пакеты (v0.7 план).
// Мирроятся байт-в-байт в libc/include/lufira/syscall.h, как и
// vfs_dirent_t/struct lufira_dirent для SYS_READDIR выше.
typedef struct {
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t inode_count;
    uint32_t free_inodes;
} lufira_statfs_t;

typedef struct {
    uint64_t total_pages;      // физическая RAM, страницы по 4KB (pmm.c)
    uint64_t used_pages;
    uint64_t heap_total_bytes; // куча ядра (heap.c)
    uint64_t heap_used_bytes;
} lufira_meminfo_t;

typedef struct {
    uint64_t total_ticks;      // сырые счётчики PIT (pit.c) — сэмплирование
    uint64_t idle_ticks;       // (два снимка + sleep между ними) делает сама
                                // пользовательская программа, как и сегодня
                                // делает kernel-native command_cpuload().
} lufira_cpuload_t;

// Одна запись снимка SYS_PSLIST (30) — v0.7 план, этап 5, под-этап 4: то же,
// что process_ps() (process.c) уже печатает построчно из process_list, плюс
// ppid/uid/cpu_ticks (те использует command_cpuload() для per-process
// разбивки — process_t.cpu_ticks уже накапливается планировщиком, см.
// комментарий у этого поля в process.h). state — сырое значение
// process_state_t; имени состояния (process_state_name()) на этой стороне
// нет, программа сама мапит числа в строки, см. userspace/user/ps.c.
typedef struct {
    uint32_t pid;
    uint32_t ppid;
    char name[32];
    uint32_t state;
    uint32_t uid;
    uint64_t cpu_ticks;
} lufira_ps_entry_t;

// Потолок длины ЛЮБОЙ NUL-терминированной строки от пользователя (filename
// для open/exec, path для chdir) — не даёт неверно терминированному буферу
// заставить нас сканировать по странице за страницей бесконечно.
#define USER_STRING_MAX 4096

// Прототипы
void syscall_init(void);
// frame_ptr — указатель на кадр регистров, сохранённый syscall_entry.S на
// ядерном стеке (см. syscall_frame_t в process.c); нужен только SYS_FORK.
uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1,
                         uint64_t arg2, uint64_t arg3,
                         uint64_t arg4, uint64_t arg5,
                         uint64_t frame_ptr);

// Открывает filename и заменяет им текущий процесс (execve()-подобно).
// Используется и SYS_EXEC, и командой shell "exec". Забирает владение
// argv/envp (форма "kmalloc на каждую строку + kmalloc на сам массив",
// NULL допустим у обоих) — освобождает их сама на любом пути, успех или
// нет (см. free_argv_envp() в elf.h).
int do_exec(const char *filename, char *argv[], char *envp[]);