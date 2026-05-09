#include "paging.h"
#include "pmm.h"
#include "../drivers/console.h"
#include <stddef.h>
#include <stdint.h>

typedef uint64_t pt_entry_t;
// Указатели на таблицы страниц (все в identity-mapped памяти)
static pt_entry_t *pml4 = NULL;
static pt_entry_t *pdpt = NULL;

// Внутренние макросы
#define PML4_INDEX(v)   (((v) >> 39) & 0x1FF)
#define PDPT_INDEX(v)   (((v) >> 30) & 0x1FF)
#define PD_INDEX(v)     (((v) >> 21) & 0x1FF)
#define PT_INDEX(v)     (((v) >> 12) & 0x1FF)

// Выровненная арифметика для флагов физического адреса
static inline uint64_t paddr_to_entry(uint64_t phys, uint64_t flags) {
    return (phys & 0x000FFFFFFFFFF000ULL) | (flags & 0xFFF);
}

static void invlpg(uint64_t addr) {
    asm volatile ("invlpg (%0)" : : "r" (addr) : "memory");
}

// Вспомогательная функция для выделения и получения таблицы
static pt_entry_t* get_or_create_table(pt_entry_t *parent, uint64_t index, int create) {
    if (!(parent[index] & PAGE_PRESENT)) {
        if (!create) return NULL;
        uint64_t phys = pmm_alloc_page();
        if (!phys) return NULL;
        parent[index] = paddr_to_entry(phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        // Обнуляем новую таблицу
        pt_entry_t *table = (pt_entry_t*)phys;
        for (int i = 0; i < 512; i++) table[i] = 0;
        return table;
    } else {
        return (pt_entry_t*)(parent[index] & 0x000FFFFFFFFFF000ULL);
    }
}

// Инициализация: identity mapping всей физической памяти 2-МБ страницами
void paging_init(void) {
    // Выделяем PML4
    uint64_t pml4_phys = pmm_alloc_page();
    if (!pml4_phys) { printf("FATAL: cannot allocate PML4\n"); while(1) __asm__("hlt"); }
    pml4 = (pt_entry_t*)pml4_phys;
    for (int i = 0; i < 512; i++) pml4[i] = 0;

    // Выделяем PDPT (одна, covers 512*1GB = 512GB, более чем достаточно)
    uint64_t pdpt_phys = pmm_alloc_page();
    if (!pdpt_phys) { printf("FATAL: cannot allocate PDPT\n"); while(1) __asm__("hlt"); }
    pdpt = (pt_entry_t*)pdpt_phys;
    for (int i = 0; i < 512; i++) pdpt[i] = 0;

    // Связываем PML4 с PDPT
    pml4[0] = paddr_to_entry(pdpt_phys, PAGE_PRESENT | PAGE_WRITE);

    // Определяем, сколько PDPTE записей нужно для покрытия всей физической памяти
    uint64_t total_pages = pmm_get_total_pages();
    uint64_t total_memory = total_pages * PAGE_SIZE;
    // Каждая запись PDPTE (с PS=1) покрывает 1 GB (2^30), но мы используем обычные PDPTE
    // и для каждой создадим PD с 2-МБ страницами.
    // Количество PDPTE = ceil(total_memory / (512 * 2*1024*1024))
    uint64_t mem_gb = (total_memory + (1ULL<<30) - 1) >> 30;
    if (mem_gb > 512) mem_gb = 512; // ограничим

    for (uint64_t gb = 0; gb < mem_gb; gb++) {
        // Выделяем PD для этого гигабайта
        uint64_t pd_phys = pmm_alloc_page();
        if (!pd_phys) { printf("FATAL: cannot allocate PD\n"); while(1) __asm__("hlt"); }
        pt_entry_t *pd = (pt_entry_t*)pd_phys;
        for (int i = 0; i < 512; i++) pd[i] = 0;

        // Связываем PDPTE с PD
        pdpt[gb] = paddr_to_entry(pd_phys, PAGE_PRESENT | PAGE_WRITE);

        // Заполняем PD 2-МБ страницами
        for (int i = 0; i < 512; i++) {
            uint64_t phys = (gb << 30) + (i << 21); // 1GB * gb + 2MB * i
            pd[i] = paddr_to_entry(phys, PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE);
        }
    }

    // Загружаем CR3
    asm volatile ("mov %0, %%cr3" : : "r" (pml4_phys) : "memory");

    // Включаем пейджинг и возможно PAE/NXE? CR0.PG уже должен быть установлен UEFI, но мы перезаписываем.
    // В long mode пейджинг всегда включен, но CR3 мы обновили, этого достаточно.
    printf("Paging initialized: identity mapping of %u MB of RAM.\n",
       (uint32_t)(total_memory / (1024*1024)));
}

static pt_entry_t* ensure_table(pt_entry_t *table, uint64_t index) {
    pt_entry_t entry = table[index];

    if (entry & PAGE_PRESENT) {
        if (entry & PAGE_HUGE) return NULL;
        return (pt_entry_t*)(entry & 0x000FFFFFFFFFF000ULL);
    }

    uint64_t phys = pmm_alloc_page();
    if (!phys) return NULL;

    pt_entry_t *new_table = (pt_entry_t*)phys;
    for (int i = 0; i < 512; i++) new_table[i] = 0;

    table[index] = (phys & 0x000FFFFFFFFFF000ULL) | PAGE_PRESENT | PAGE_WRITE;
    return new_table;
}

int map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    pt_entry_t *pdpt_table = ensure_table(pml4, PML4_INDEX(virt));
    if (!pdpt_table) return -1;

    pt_entry_t *pd_table = ensure_table(pdpt_table, PDPT_INDEX(virt));
    if (!pd_table) return -1;

    pt_entry_t *pt_table = ensure_table(pd_table, PD_INDEX(virt));
    if (!pt_table) return -1;

    pt_table[PT_INDEX(virt)] = (phys & 0x000FFFFFFFFFF000ULL) | (flags & 0xFFF) | PAGE_PRESENT;
    invlpg(virt);
    return 0;
}

void unmap_page(uint64_t virt) {
    pt_entry_t *pml4e = &pml4[PML4_INDEX(virt)];
    if (!(*pml4e & PAGE_PRESENT)) return;
    pt_entry_t *pdpt_table = (pt_entry_t*)(*pml4e & 0x000FFFFFFFFFF000ULL);
    pt_entry_t *pdpte = &pdpt_table[PDPT_INDEX(virt)];
    if (!(*pdpte & PAGE_PRESENT)) return;
    pt_entry_t *pd_table = (pt_entry_t*)(*pdpte & 0x000FFFFFFFFFF000ULL);
    pt_entry_t *pde = &pd_table[PD_INDEX(virt)];
    if (!(*pde & PAGE_PRESENT)) return;
    pt_entry_t *pt_table = (pt_entry_t*)(*pde & 0x000FFFFFFFFFF000ULL);
    pt_entry_t *pte = &pt_table[PT_INDEX(virt)];
    *pte = 0;
    invlpg(virt);
}

uint64_t get_physical_address(uint64_t virt) {
    pt_entry_t *pml4e = &pml4[PML4_INDEX(virt)];
    if (!(*pml4e & PAGE_PRESENT)) return 0;
    pt_entry_t *pdpt_table = (pt_entry_t*)(*pml4e & 0x000FFFFFFFFFF000ULL);
    pt_entry_t *pdpte = &pdpt_table[PDPT_INDEX(virt)];
    if (!(*pdpte & PAGE_PRESENT)) return 0;
    pt_entry_t *pd_table = (pt_entry_t*)(*pdpte & 0x000FFFFFFFFFF000ULL);
    pt_entry_t *pde = &pd_table[PD_INDEX(virt)];
    if (!(*pde & PAGE_PRESENT)) return 0;
    if (*pde & PAGE_HUGE) {
        // 2MB page
        uint64_t phys = (*pde & 0x000FFFFFFFFFF000ULL) + (virt & 0x1FFFFF);
        return phys;
    } else {
        pt_entry_t *pt_table = (pt_entry_t*)(*pde & 0x000FFFFFFFFFF000ULL);
        pt_entry_t *pte = &pt_table[PT_INDEX(virt)];
        if (!(*pte & PAGE_PRESENT)) return 0;
        return (*pte & 0x000FFFFFFFFFF000ULL) + (virt & 0xFFF);
    }
}