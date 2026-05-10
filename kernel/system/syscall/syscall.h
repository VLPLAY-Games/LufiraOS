#pragma once

#include "lib/types.h"

// Номера системных вызовов
#define SYS_WRITE   0
#define SYS_READ    1
#define SYS_EXIT    2
#define SYS_GETPID  3
#define SYS_YIELD   4
#define SYS_GETTICK 5

// Прототип обработчика syscall
void syscall_init(void);
uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, 
                         uint64_t arg2, uint64_t arg3, 
                         uint64_t arg4, uint64_t arg5);