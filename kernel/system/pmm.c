#include "pmm.h"
#include "../drivers/console.h"   // для printf
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096
#define BITMAP_ENTRY_SIZE 8

static uint8_t *bitmap = NULL;
static uint64_t total_pages = 0;
static uint64_t used_pages = 0;

// Внутренние функции работы с bitmap (очень простые)
static inline void bitmap_set(uint64_t page) {
    bitmap[page / BITMAP_ENTRY_SIZE] |= (1 << (page % BITMAP_ENTRY_SIZE));
}

static inline void bitmap_clear(uint64_t page) {
    bitmap[page / BITMAP_ENTRY_SIZE] &= ~(1 << (page % BITMAP_ENTRY_SIZE));
}

static inline int bitmap_test(uint64_t page) {
    return (bitmap[page / BITMAP_ENTRY_SIZE] & (1 << (page % BITMAP_ENTRY_SIZE))) != 0;
}

static uint64_t find_first_free_pages(uint64_t num) {
    uint64_t start = 0, count = 0;
    for (uint64_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            if (count == 0) start = i;
            count++;
            if (count == num) return start;
        } else {
            count = 0;
        }
    }
    return (uint64_t)-1;
}

// Первый проход: подсчёт максимального числа страниц и выбор места для bitmap
void pmm_init(void* memory_map, uint64_t map_size, uint32_t desc_size,
              uint64_t kernel_base, uint64_t kernel_size)
{
    EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR*)memory_map;
    uint64_t desc_count = map_size / desc_size;

    // Находим максимальный физический адрес
    uint64_t max_phys = 0;

    for (uint64_t i = 0; i < desc_count; i++) {
        uint64_t end =
            desc[i].PhysicalStart +
            desc[i].NumberOfPages * PAGE_SIZE;

        if (end > max_phys)
            max_phys = end;
    }

    total_pages = max_phys / PAGE_SIZE;

    if (max_phys % PAGE_SIZE)
        total_pages++;

    // Размер bitmap в байтах
    uint64_t bitmap_size =
        (total_pages + BITMAP_ENTRY_SIZE - 1) / BITMAP_ENTRY_SIZE;

    // Ищем место под bitmap
    uint64_t bitmap_phys = 0;

    for (uint64_t i = 0; i < desc_count; i++) {

        if (desc[i].Type != 7) // EfiConventionalMemory
            continue;

        uint64_t block_start = desc[i].PhysicalStart;
        uint64_t block_size  = desc[i].NumberOfPages * PAGE_SIZE;

        // bitmap должен помещаться целиком
        if (block_size >= bitmap_size) {
            bitmap_phys = block_start;
            break;
        }
    }

    if (!bitmap_phys) {
        printf("FATAL: Cannot find memory for PMM bitmap!\n");
        while (1) __asm__("hlt");
    }

    printf("bitmap_phys=%lx bitmap_size=%lx\n",
           bitmap_phys, bitmap_size);

    // IMPORTANT:
    // Сейчас paging ещё identity mapped,
    // поэтому physical == virtual.
    bitmap = (uint8_t*)bitmap_phys;

    // Обнуляем bitmap
    for (uint64_t i = 0; i < bitmap_size; i++)
        bitmap[i] = 0;

    // Сначала помечаем ВСЁ как занятое
    for (uint64_t i = 0; i < total_pages; i++)
        bitmap_set(i);

    // Освобождаем только EfiConventionalMemory
    for (uint64_t i = 0; i < desc_count; i++) {

        if (desc[i].Type != 7)
            continue;

        uint64_t start_page =
            desc[i].PhysicalStart / PAGE_SIZE;

        uint64_t pages =
            desc[i].NumberOfPages;

        for (uint64_t p = 0; p < pages; p++)
            bitmap_clear(start_page + p);
    }

    // =====================================================
    // РЕЗЕРВ LOW MEMORY (0..1MB)
    // =====================================================

    // 256 страниц * 4096 = 1MB
    for (uint64_t i = 0; i < 256; i++)
        bitmap_set(i);

    // =====================================================
    // РЕЗЕРВ СТРАНИЦ BITMAP
    // =====================================================

    uint64_t bitmap_start_page =
        bitmap_phys / PAGE_SIZE;

    uint64_t bitmap_pages =
        (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t p = 0; p < bitmap_pages; p++)
        bitmap_set(bitmap_start_page + p);

    // =====================================================
    // РЕЗЕРВ ЯДРА
    // =====================================================

    uint64_t kernel_start_page =
        kernel_base / PAGE_SIZE;

    uint64_t kernel_pages =
        (kernel_size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t p = 0; p < kernel_pages; p++)
        bitmap_set(kernel_start_page + p);

    // =====================================================
    // ПОДСЧЁТ USED PAGES
    // =====================================================

    used_pages = 0;

    for (uint64_t i = 0; i < total_pages; i++) {
        if (bitmap_test(i))
            used_pages++;
    }

    printf("PMM initialized: %lu pages total, %lu used, %lu free.\n",
           total_pages,
           used_pages,
           total_pages - used_pages);
}

uint64_t pmm_alloc_page(void) {
    uint64_t page = find_first_free_pages(1);
    if (page == (uint64_t)-1) return 0;
    bitmap_set(page);
    used_pages++;
    return page * PAGE_SIZE;
}

void pmm_free_page(uint64_t phys) {
    uint64_t page = phys / PAGE_SIZE;
    if (page >= total_pages) return;
    bitmap_clear(page);
    used_pages--;
}

uint64_t pmm_get_total_pages(void) {
    return total_pages;
}