#pragma once

#include "lib/types.h"
#include "lib/stddef.h"
#include "bootinfo.h"
#include "drivers/console/console.h"

#define PAGE_SIZE            4096
#define PAGE_PRESENT         0x001
#define PAGE_WRITE           0x002
#define PAGE_USER            0x004
#define PAGE_PWT             0x008
#define PAGE_PCD             0x010
#define PAGE_ACCESSED        0x020
#define PAGE_DIRTY           0x040
#define PAGE_HUGE            0x080
#define PAGE_GLOBAL          0x100
#define PAGE_NX              (1ULL << 63)

// Инициализация страничной адресации
void paging_init(BootInfo* bi);

// Отобразить виртуальную страницу на физический адрес
int map_page(uint64_t virt, uint64_t phys, uint64_t flags);

// Снять отображение
void unmap_page(uint64_t virt);

// Получить физический адрес по виртуальному
uint64_t get_physical_address(uint64_t virt);

// Получить физический адрес текущего PML4
uint64_t get_current_pml4(void);