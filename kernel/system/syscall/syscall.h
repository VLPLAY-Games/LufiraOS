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