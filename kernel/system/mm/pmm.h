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

// Освободить страницу
void pmm_free_page(uint64_t phys);

// Общее количество физических страниц (для paging)
uint64_t pmm_get_total_pages(void);
