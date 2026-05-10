#pragma once

#include <stddef.h>

#define KERNEL_HEAP_START 0xFFFF900000000000ULL

void heap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);
