#pragma once

#include "lib/types.h"
#include "lib/stddef.h"

// EFI-совместимый дескриптор памяти (из bootloader)
typedef struct {
    uint32_t Type;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

// Инициализация PMM: передаётся карта памяти и информация о ядре
void pmm_init(void* memory_map, uint64_t map_size, uint32_t desc_size,
              uint64_t kernel_base, uint64_t kernel_size);

// Выделить одну физическую страницу (возвращает физический адрес, 0 при ошибке)
uint64_t pmm_alloc_page(void);

// Выделить count ФИЗИЧЕСКИ ПОДРЯД ИДУЩИХ страниц (возвращает физический
// адрес первой, 0 при ошибке — в том числе если подряд идущих count
// свободных страниц не нашлось вообще). В отличие от pmm_alloc_page(),
// который ничего не гарантирует про соседние вызовы, это нужно устройствам
// без scatter-gather на приём (см. kernel/drivers/net/rtl8139.c — кольцевой
// приёмный буфер RTL8139 требует одного непрерывного физического региона).
// Освобождать по одной странице через pmm_free_page() — отдельного
// "free_contiguous" не нужно, эта функция сама только выставляет биты.
uint64_t pmm_alloc_contiguous_pages(uint32_t count);

// Освободить страницу
void pmm_free_page(uint64_t phys);

// Общее количество физических страниц (для paging)
uint64_t pmm_get_total_pages(void);
