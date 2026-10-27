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

// Инициализация PMM: карта памяти, регион ядра и ДОПОЛНИТЕЛЬНЫЙ регион для
// резервирования (reserved_base==0/reserved_size==0 — ничего, кроме ядра,
// не резервировать; с v1.0.0 kernel.c всегда зовёт с 0, 0 — см. pmm.c).
void pmm_init(void* memory_map, uint64_t map_size, uint32_t desc_size,
              uint64_t kernel_base, uint64_t kernel_size,
              uint64_t reserved_base, uint64_t reserved_size);

// Выделить одну физическую страницу (возвращает физический адрес, 0 при ошибке)
uint64_t pmm_alloc_page(void);

// Выделить count ФИЗИЧЕСКИ ПОДРЯД идущих страниц (адрес первой, 0 при
// ошибке/нехватке). В отличие от pmm_alloc_page() это нужно устройствам без
// scatter-gather (RTL8139 — kernel/drivers/net/rtl8139.c, непрерывный
// кольцевой буфер приёма). Освобождать по одной странице через pmm_free_page().
uint64_t pmm_alloc_contiguous_pages(uint32_t count);

// Освободить страницу
void pmm_free_page(uint64_t phys);

// Общее количество физических страниц (для paging)
uint64_t pmm_get_total_pages(void);

// Количество занятых страниц прямо сейчас (для команды free)
uint64_t pmm_get_used_pages(void);
