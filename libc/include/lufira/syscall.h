// lufira/syscall.h — обёртки над системными вызовами LufiraOS.
//
// АБИ (зеркалит kernel/system/syscall/syscall_entry.S и syscall.h):
//   rax = номер syscall'а, аргументы в rdi,rsi,rdx,r10,r8 (r10, а НЕ rcx —
//   сама инструкция syscall затирает rcx/r11), результат в rax.
//
// Соглашение об ошибках (факт АБИ именно ЭТОГО ядра, не общий POSIX): все
// сисколлы, которые вообще могут завершиться ошибкой, возвращают маленький
// отрицательный код (-ENOENT, -EFAULT и т.п., см. ниже) как (uint64_t)-CODE.
// Любое настоящее успешное значение (счётчик байт, адрес из mmap — тот
// лежит высоко в канонической пользовательской области) при интерпретации
// как int64_t положительно, так что проверка "(long)ret < 0" везде ниже
// корректно отличает ошибку от успеха.
#pragma once

#include <stdint.h>

// Номера системных вызовов — как в kernel/system/syscall/syscall.h.
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
#define SYS_SET_FOREGROUND 31
#define SYS_SU 32
#define SYS_MOUNT 33
#define SYS_UNMOUNT 34
#define SYS_REBOOT 35
#define SYS_SHUTDOWN 36
#define SYS_DEVMODE 37
#define SYS_USERADD 38
#define SYS_GROUPADD 39
#define SYS_PASSWD 40
#define SYS_USB_COUNT 41
#define SYS_USB_INFO 42
#define SYS_USB_READ 43
#define SYS_USB_WRITE 44
#define SYS_POLL 45
#define SYS_SIGACTION 46
#define SYS_SIGRETURN 47
#define SYS_ALARM 48
#define SYS_GET_FOREGROUND 49

// Флаги sys_open().
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   4
#define O_TRUNC   8
#define O_APPEND  16

// Флаги sys_seek().
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

// Флаги sys_mmap().
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20

// Коды ошибок — ровно то подмножество, которое ядро реально возвращает.
#define EPERM    1
#define ENOENT   2
#define EACCES   13
#define EFAULT   14
#define ENOTDIR  20
#define EINVAL   22
#define ERANGE   34

static inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    asm volatile("syscall"
        : "=a"(ret)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return ret;
}

static inline long sys_write(int fd, const void *buf, unsigned long count) {
    return __syscall5(SYS_WRITE, fd, (long)buf, (long)count, 0, 0);
}
static inline long sys_read(int fd, void *buf, unsigned long count) {
    return __syscall5(SYS_READ, fd, (long)buf, (long)count, 0, 0);
}
static inline __attribute__((noreturn)) void sys_exit(int code) {
    __syscall5(SYS_EXIT, code, 0, 0, 0, 0);
    for (;;) { } // sys_exit не возвращается; тело — только чтобы компилятор поверил в noreturn
}
static inline long sys_getpid(void) {
    return __syscall5(SYS_GETPID, 0, 0, 0, 0, 0);
}
static inline long sys_yield(void) {
    return __syscall5(SYS_YIELD, 0, 0, 0, 0, 0);
}
static inline long sys_gettick(void) {
    return __syscall5(SYS_GETTICK, 0, 0, 0, 0, 0);
}
static inline long sys_open(const char *path, int flags, int mode) {
    return __syscall5(SYS_OPEN, (long)path, flags, mode, 0, 0);
}
static inline long sys_close(int fd) {
    return __syscall5(SYS_CLOSE, fd, 0, 0, 0, 0);
}
static inline long sys_lseek(int fd, long offset, int whence) {
    return __syscall5(SYS_SEEK, fd, offset, whence, 0, 0);
}
static inline long sys_mmap(void *addr, unsigned long length, int prot, int flags, int fd) {
    return __syscall5(SYS_MMAP, (long)addr, (long)length, prot, flags, fd);
}
static inline long sys_munmap(void *addr, unsigned long length) {
    return __syscall5(SYS_MUNMAP, (long)addr, (long)length, 0, 0, 0);
}
static inline long sys_exec(const char *filename, char *const argv[], char *const envp[]) {
    return __syscall5(SYS_EXEC, (long)filename, (long)argv, (long)envp, 0, 0);
}
static inline long sys_fork(void) {
    return __syscall5(SYS_FORK, 0, 0, 0, 0, 0);
}
static inline long sys_wait(long pid, int *status, int options) {
    return __syscall5(SYS_WAIT, pid, (long)status, options, 0, 0);
}
static inline long sys_getcwd(char *buf, unsigned long size) {
    return __syscall5(SYS_GETCWD, (long)buf, (long)size, 0, 0, 0);
}
static inline long sys_chdir(const char *path) {
    return __syscall5(SYS_CHDIR, (long)path, 0, 0, 0, 0);
}
static inline long sys_msleep(unsigned long milliseconds) {
    return __syscall5(SYS_SLEEP, (long)milliseconds, 0, 0, 0, 0);
}
// Номера сигналов — те же значения, что в kernel/system/process/process.h.
#define SIGINT  2
#define SIGALRM 14
#define SIGKILL 9
#define SIGTERM 15
#define SIGCONT 18
#define SIGSTOP 19

static inline long sys_kill(long pid, int sig) {
    return __syscall5(SYS_KILL, pid, sig, 0, 0, 0);
}
static inline long sys_pipe(int fds[2]) {
    return __syscall5(SYS_PIPE, (long)fds, 0, 0, 0, 0);
}
static inline long sys_chmod(const char *path, int mode) {
    return __syscall5(SYS_CHMOD, (long)path, mode, 0, 0, 0);
}
static inline long sys_chown(const char *path, int uid, int gid) {
    return __syscall5(SYS_CHOWN, (long)path, uid, gid, 0, 0);
}
static inline long sys_getuid(void) {
    return __syscall5(SYS_GETUID, 0, 0, 0, 0, 0);
}
static inline long sys_getgid(void) {
    return __syscall5(SYS_GETGID, 0, 0, 0, 0, 0);
}
static inline long sys_mkdir(const char *path, int mode) {
    return __syscall5(SYS_MKDIR, (long)path, mode, 0, 0, 0);
}
static inline long sys_rmdir(const char *path) {
    return __syscall5(SYS_RMDIR, (long)path, 0, 0, 0, 0);
}
static inline long sys_unlink(const char *path) {
    return __syscall5(SYS_UNLINK, (long)path, 0, 0, 0, 0);
}

// Зеркалит vfs_dirent_t (kernel/fs/vfs/vfs.h) байт-в-байт: type — обычный
// (не short/packed) C enum на этом тулчейне, то есть ровно 4 байта, как и
// uint32_t здесь — тот же размер и раскладка полей, что видит ядро при
// записи через sys_readdir().
#define LUFIRA_FT_REGULAR   0
#define LUFIRA_FT_DIRECTORY 1
#define LUFIRA_FT_CHARDEV   2
#define LUFIRA_FT_BLOCKDEV  3
#define LUFIRA_FT_PIPE      4
#define LUFIRA_FT_SYMLINK   5

struct lufira_dirent {
    uint32_t ino;
    uint32_t type;
    char name[256];
};

static inline long sys_readdir(int fd, struct lufira_dirent *out) {
    return __syscall5(SYS_READDIR, fd, (long)out, 0, 0, 0);
}

// Мирроят lufira_statfs_t/lufira_meminfo_t/lufira_cpuload_t (kernel/system/
// syscall/syscall.h) байт-в-байт — v0.7 этап 1 (вынос du/df/free/cpuload).
struct lufira_statfs {
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t inode_count;
    uint32_t free_inodes;
};

struct lufira_meminfo {
    uint64_t total_pages;
    uint64_t used_pages;
    uint64_t heap_total_bytes;
    uint64_t heap_used_bytes;
};

struct lufira_cpuload {
    uint64_t total_ticks;
    uint64_t idle_ticks;
};

static inline long sys_statfs(struct lufira_statfs *out) {
    return __syscall5(SYS_STATFS, (long)out, 0, 0, 0, 0);
}
static inline long sys_meminfo(struct lufira_meminfo *out) {
    return __syscall5(SYS_MEMINFO, (long)out, 0, 0, 0, 0);
}
static inline long sys_cpuload(struct lufira_cpuload *out) {
    return __syscall5(SYS_CPULOAD, (long)out, 0, 0, 0, 0);
}

// Мирроит lufira_ps_entry_t (kernel/system/syscall/syscall.h) байт-в-байт —
// v0.7 план, этап 5, под-этап 4 ("ps").
struct lufira_ps_entry {
    uint32_t pid;
    uint32_t ppid;
    char name[32];
    uint32_t state;
    uint32_t uid;
    uint64_t cpu_ticks;
};

// process_state_t (process.h) — значения state в struct lufira_ps_entry.
#define LUFIRA_PROCESS_READY      0
#define LUFIRA_PROCESS_RUNNING    1
#define LUFIRA_PROCESS_BLOCKED    2
#define LUFIRA_PROCESS_SLEEPING   3
#define LUFIRA_PROCESS_TERMINATED 4
#define LUFIRA_PROCESS_STOPPED    5

static inline long sys_pslist(struct lufira_ps_entry *out, unsigned long max_count) {
    return __syscall5(SYS_PSLIST, (long)out, (long)max_count, 0, 0, 0);
}

// pid == 0 снимает foreground (Ctrl+C больше никого не целит) — v0.7 план,
// этап 5, под-этап 6. pid должен быть ПОТОМКОМ вызывающего (ядро
// проверяет это по всей цепочке ppid до корня, не только прямое родство —
// см. process_set_foreground(), генерализация v0.8-мост, пункт 7).
static inline long sys_set_foreground(long pid) {
    return __syscall5(SYS_SET_FOREGROUND, pid, 0, 0, 0, 0);
}

// Текущий foreground_pid (0 — не выставлен) — v0.8-мост, пункт 7, спутник
// к sys_set_foreground() выше, который сам раньше не давал способа
// прочитать значение.
static inline long sys_get_foreground(void) {
    return __syscall5(SYS_GET_FOREGROUND, 0, 0, 0, 0, 0);
}

// 0 при успехе (меняет uid/gid вызывающего процесса), иначе -errno
// (-ENOENT — нет такого пользователя, -EPERM — неверный пароль). Пароль
// проверяется В ЯДРЕ — root проходит без пароля, остальным он обязателен.
static inline long sys_su(const char *username, const char *password) {
    return __syscall5(SYS_SU, (long)username, (long)password, 0, 0, 0);
}

// Монтирует usb-устройство usb_index как FAT под префиксом prefix
// ("/mnt/usb0") — обычные open/read/write/mkdir/unlink/readdir начинают
// видеть файлы под этим префиксом (только корневой уровень флешки — см.
// комментарий у SYS_MOUNT в kernel/system/syscall/syscall.h). >=0 при
// успехе, иначе -errno-подобный код оттуда же.
static inline long sys_mount(const char *prefix, long usb_index) {
    return __syscall5(SYS_MOUNT, (long)prefix, usb_index, 0, 0, 0);
}

static inline long sys_unmount(const char *prefix) {
    return __syscall5(SYS_UNMOUNT, (long)prefix, 0, 0, 0, 0);
}

// Требуют root; не возвращаются при успехе (-errno при неудаче).
static inline long sys_reboot(void) {
    return __syscall5(SYS_REBOOT, 0, 0, 0, 0, 0);
}

static inline long sys_shutdown(void) {
    return __syscall5(SYS_SHUTDOWN, 0, 0, 0, 0, 0);
}

// mode: 0 — прочитать состояние (возвращает 0/1), 1 — включить, 2 — выключить.
static inline long sys_devmode(long mode) {
    return __syscall5(SYS_DEVMODE, mode, 0, 0, 0, 0);
}

// Root-only. group == NULL — своя группа с именем username (как настоящий
// Linux useradd по умолчанию). 0 при успехе, иначе -errno-подобный код
// (см. комментарий у SYS_USERADD в kernel/system/syscall/syscall.h).
static inline long sys_useradd(const char *username, const char *password, const char *group) {
    return __syscall5(SYS_USERADD, (long)username, (long)password, (long)group, 0, 0);
}

// Root-only.
static inline long sys_groupadd(const char *groupname) {
    return __syscall5(SYS_GROUPADD, (long)groupname, 0, 0, 0, 0);
}

// username == NULL — сменить свой собственный пароль (без проверки прав).
// Ненулевой username — сброс пароля ЛЮБОГО пользователя, root-only.
static inline long sys_passwd(const char *username, const char *new_password) {
    return __syscall5(SYS_PASSWD, (long)username, (long)new_password, 0, 0, 0);
}

// Байт-в-байт зеркало lufira_usb_info_t (kernel/system/syscall/syscall.h).
struct lufira_usb_info {
    uint32_t max_lba;
    uint32_t block_size;
};

static inline long sys_usb_count(void) {
    return __syscall5(SYS_USB_COUNT, 0, 0, 0, 0, 0);
}

static inline long sys_usb_info(long index, struct lufira_usb_info *out) {
    return __syscall5(SYS_USB_INFO, index, (long)out, 0, 0, 0);
}

// buf_size должен быть >= block_size реального устройства (см. sys_usb_info()),
// иначе -EINVAL. Возвращает число прочитанных/записанных байт при успехе.
static inline long sys_usb_read(long index, unsigned long lba, void *buf, unsigned long buf_size) {
    return __syscall5(SYS_USB_READ, index, (long)lba, (long)buf, (long)buf_size, 0);
}

// Root-only (прямая запись по LBA необратима) — см. комментарий у
// SYS_USB_WRITE в kernel/system/syscall/syscall.h.
static inline long sys_usb_write(long index, unsigned long lba, const void *buf, unsigned long buf_size) {
    return __syscall5(SYS_USB_WRITE, index, (long)lba, (long)buf, (long)buf_size, 0);
}

// SYS_POLL (45) — v0.8-мост, пункт 3. Байт-в-байт зеркало lufira_pollfd_t
// (kernel/system/syscall/syscall.h).
#define LUFIRA_POLLIN  1
#define LUFIRA_POLLOUT 2

struct lufira_pollfd {
    int fd;
    int events;
    int revents;
};

// timeout_ms: 0 — опросить и вернуться сразу, >0 — ждать не больше
// стольки мс, <0 — ждать неограниченно. Возвращает число fd с ненулевым
// revents, 0 при таймауте, иначе отрицательный код ошибки.
static inline long sys_poll(struct lufira_pollfd *fds, unsigned long nfds, long timeout_ms) {
    return __syscall5(SYS_POLL, (long)fds, (long)nfds, timeout_ms, 0, 0);
}

// SYS_SIGACTION (46) / SYS_SIGRETURN (47) — v0.8-мост, пункт 5. SIGINT/
// SIGTERM/SIGALRM (см. комментарий у SYS_SIGACTION, kernel/system/syscall/
// syscall.h) — SIGKILL/SIGSTOP/SIGCONT нельзя поймать.
//
// ВАЖНО, контракт этого ABI (НЕ настоящий POSIX sigaction/sigreturn):
// обработчик обязан заканчиваться вызовом sys_sigreturn(), а не обычным
// C "return" — у этого ядра нет сигнального трамплина в памяти процесса
// (sys_sigreturn() просто подменяет кадр ВОЗВРАТА ИЗ ЭТОГО САМОГО
// вызова на сохранённое состояние прерванного кода, см. process_sigreturn()
// в kernel/system/process/process.c) — обычный "ret" из обработчика
// попытался бы вернуться туда же, откуда его вызвал syscall_handler()
// (т.е. никуда конкретного, скорее всего крах).
//
//   void on_sigterm(int sig) {
//       ...
//       sys_sigreturn(); // НЕ "return;"
//   }
//   sys_sigaction(SIGTERM, (void*)on_sigterm);
static inline long sys_sigaction(int sig, void (*handler)(int)) {
    return __syscall5(SYS_SIGACTION, sig, (long)handler, 0, 0, 0);
}

__attribute__((noreturn)) static inline void sys_sigreturn(void) {
    __syscall5(SYS_SIGRETURN, 0, 0, 0, 0, 0);
    __builtin_unreachable(); // sigreturn никогда не "возвращается" сюда обычным путём
}

// SYS_ALARM (48) — v0.8-мост, пункт 6. milliseconds==0 снимает уже
// взведённый будильник. Доставляется ОДИН раз, не периодически — для
// повтора (например, частота кадров будущего GUI) обработчик сам
// переустанавливает его в конце:
//
//   void on_tick(int sig) {
//       ...нарисовать кадр...
//       sys_alarm(16); // ~60 Гц
//       sys_sigreturn();
//   }
//   sys_sigaction(SIGALRM, on_tick);
//   sys_alarm(16);
static inline long sys_alarm(unsigned long milliseconds) {
    return __syscall5(SYS_ALARM, (long)milliseconds, 0, 0, 0, 0);
}
