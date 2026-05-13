#pragma once

#include "lib/stddef.h"

#define KERNEL_HEAP_START   0xFFFF900000000000ULL
#define KERNEL_HEAP_SIZE    (16 * 1024 * 1024) // 16 MB
#define KERNEL_HEAP_END     (KERNEL_HEAP_START + KERNEL_HEAP_SIZE)

void heap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);