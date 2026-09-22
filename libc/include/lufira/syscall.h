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
#define ENOENT   2
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
static inline long sys_kill(long pid, int sig) {
    return __syscall5(SYS_KILL, pid, sig, 0, 0, 0);
}
static inline long sys_pipe(int fds[2]) {
    return __syscall5(SYS_PIPE, (long)fds, 0, 0, 0, 0);
}
