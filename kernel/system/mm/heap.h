#pragma once

#include "lib/stddef.h"

#define KERNEL_HEAP_START   0xFFFF900000000000ULL
#define KERNEL_HEAP_SIZE    (16 * 1024 * 1024) // 16 MB
#define KERNEL_HEAP_END     (KERNEL_HEAP_START + KERNEL_HEAP_SIZE)

void heap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);

// Сумма USED/FREE блоков кучи в байтах (для команды free).
void heap_get_stats(uint64_t *used_out, uint64_t *free_out);