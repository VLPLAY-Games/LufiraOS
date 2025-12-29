#ifndef KERNEL_H
#define KERNEL_H

#include <efi.h>

// Основные функции ядра
UINT64 kernel_main(void);
void kernel_initialize(void);
void kernel_panic(const char *message);

// Структуры ядра
typedef struct {
    UINT64 memory_size;
    UINT64 kernel_start;
    UINT64 kernel_end;
} KERNEL_INFO;

#endif // KERNEL_H