#include "pmm.h"
#include "../drivers/console.h"   // для printf
#include <stddef.h>
#include <stdint.h>
#include "system/log.h"

#define PAGE_SIZE 4096
#define BITMAP_ENTRY_SIZE 8
#define EfiConventionalMemory 7

static uint8_t *bitmap = NULL;
static uint64_t total_pages = 0;
static uint64_t used_pages = 0;
static uint64_t next_free_page = 256;

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

// Первый проход: подсчёт максимального числа страниц и выбор места для bitmap
void pmm_init(void* memory_map, uint64_t map_size, uint32_t desc_size,
              uint64_t kernel_base, uint64_t kernel_size)
{
    uint8_t *map = (uint8_t*)memory_map;
    uint64_t desc_count = map_size / desc_size;
    LOG_PENDING("Initializing PMM...");

    // =====================================================
    // НАХОДИМ МАКСИМАЛЬНЫЙ ФИЗИЧЕСКИЙ АДРЕС
    // =====================================================

    uint64_t max_phys = 0;
    for (uint64_t i = 0; i < desc_count; i++) {
        EFI_MEMORY_DESCRIPTOR *d =
            (EFI_MEMORY_DESCRIPTOR*)(map + i * desc_size);
        if (d->Type != EfiConventionalMemory)
            continue;
        uint64_t end =
            d->PhysicalStart +
            d->NumberOfPages * PAGE_SIZE;
        if (end > max_phys)
            max_phys = end;
    }

    total_pages = max_phys / PAGE_SIZE;

    if (max_phys % PAGE_SIZE)
        total_pages++;

    // =====================================================
    // РАЗМЕР BITMAP
    // =====================================================

    uint64_t bitmap_size =
        (total_pages + BITMAP_ENTRY_SIZE - 1) / BITMAP_ENTRY_SIZE;

    // =====================================================
    // ПОИСК МЕСТА ПОД BITMAP
    // =====================================================

    uint64_t bitmap_phys = 0;
    uint64_t best_size = 0;

    uint64_t kernel_end =
        kernel_base + kernel_size;

    for (uint64_t i = 0; i < desc_count; i++) {

        EFI_MEMORY_DESCRIPTOR *d =
            (EFI_MEMORY_DESCRIPTOR*)(map + i * desc_size);

        if (d->Type != EfiConventionalMemory)
            continue;

        uint64_t block_start =
            d->PhysicalStart;

        uint64_t block_size =
            d->NumberOfPages * PAGE_SIZE;

        uint64_t block_end =
            block_start + block_size;

        // не размещать bitmap поверх ядра
        if (!(block_end <= kernel_base ||
              block_start >= kernel_end))
        {
            continue;
        }

        // выбираем самый большой блок
        if (block_size >= bitmap_size &&
            block_size > best_size)
        {
            best_size = block_size;
            bitmap_phys = block_start;
        }
    }

    if (!bitmap_phys) {
        LOG_DONE_FAIL("PMM: Cannot find memory for bitmap");
        while (1) __asm__("hlt");
    }

    printf("bitmap_phys=0x%x bitmap_size=%u bytes\n", (uint32_t)bitmap_phys, (uint32_t)bitmap_size);

    // =====================================================
    // PHYSICAL == VIRTUAL (identity mapping)
    // =====================================================

    bitmap = (uint8_t*)bitmap_phys;

    // =====================================================
    // ОЧИСТКА BITMAP
    // =====================================================

    for (uint64_t i = 0; i < bitmap_size; i++)
        bitmap[i] = 0;

    // =====================================================
    // СНАЧАЛА ВСЁ ЗАНЯТО
    // =====================================================

    for (uint64_t i = 0; i < total_pages; i++)
        bitmap_set(i);

    // =====================================================
    // ОСВОБОЖДАЕМ EfiConventionalMemory
    // =====================================================

    for (uint64_t i = 0; i < desc_count; i++) {

        EFI_MEMORY_DESCRIPTOR *d =
            (EFI_MEMORY_DESCRIPTOR*)(map + i * desc_size);

        if (d->Type != EfiConventionalMemory)
            continue;

        uint64_t start_page =
            d->PhysicalStart / PAGE_SIZE;

        uint64_t pages =
            d->NumberOfPages;

        for (uint64_t p = 0; p < pages; p++)
            bitmap_clear(start_page + p);
    }

    // =====================================================
    // РЕЗЕРВ LOW MEMORY (0..1MB)
    // =====================================================

    for (uint64_t i = 0; i < 256; i++)
        bitmap_set(i);

    // =====================================================
    // РЕЗЕРВ ПЕРВЫХ 2MB (identity mapping area)
    // =====================================================
    for (uint64_t i = 0; i < 512; i++)
        bitmap_set(i);

    // =====================================================
    // РЕЗЕРВ BITMAP
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

    next_free_page = 512;

    uint64_t free_pages = total_pages - used_pages;
    LOG_DONE_OK("PMM: %u MB total, %u MB used, %u MB free",
        (uint32_t)(total_pages * 4 / 1024),
        (uint32_t)(used_pages * 4 / 1024),
        (uint32_t)(free_pages * 4 / 1024),
        (uint32_t)free_pages);
}

uint64_t pmm_alloc_page(void) {
    for (uint64_t i = next_free_page; i < total_pages; i++) {

        if (!bitmap_test(i)) {

            bitmap_set(i);
            used_pages++;

            next_free_page = i + 1;

            return i * PAGE_SIZE;
        }
    }

    return 0;
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