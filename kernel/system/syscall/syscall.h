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

// Прототипы
void syscall_init(void);
uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, 
                         uint64_t arg2, uint64_t arg3, 
                         uint64_t arg4, uint64_t arg5);