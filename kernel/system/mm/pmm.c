#include "pmm.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"
#include "lib/types.h"
#include "log.h"

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
              uint64_t kernel_base, uint64_t kernel_size,
              uint64_t reserved_base, uint64_t reserved_size)
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
    // РЕЗЕРВ ОБРАЗА ДИСКА В RAM (bi->FATImageBase)
    // =====================================================
    //
    // Бутлоадер грузит ВЕСЬ диск (ESP + LufiraFS) одним куском в RAM через
    // UEFI AllocatePages(AllocateAnyPages, EfiLoaderData, ...) (см.
    // boot/loaders/fat_loader.c) — а LufiraFS (lufirafs.c) потом читает и
    // пишет прямо в эту память как в единственный источник правды (fs->image),
    // без какого-либо отдельного кеша. Раньше pmm_init() ничего не знал об
    // этом регионе и резервировал только kernel_base/kernel_size — если UEFI
    // память под эту аллокацию попадала в карту как EfiConventionalMemory
    // (или если прошивка переиспользовала адреса, которые сама же разметила
    // неточно), pmm_alloc_page() рано или поздно выдавал процессу физическую
    // страницу, которая на самом деле являлась частью образа ФС на диске.
    // Процесс писал в "свою" страницу как ни в чём не бывало — а на самом
    // деле тихо портил LufiraFS прямо в памяти: случайные inode/dirent/блоки
    // переставали совпадать с тем, что там должно быть. Воспроизводилось как
    // "lufirafs_lookup() вдруг не находит существующий файл" после
    // достаточного числа подряд идущих fork()+exec() (каждый клонирует/дерево
    // строит много новых страниц, так и добирались до этого региона). Фикс —
    // тот же приём, что уже есть для kernel_base/kernel_size: резервируем
    // явно, раз и навсегда, до первого же pmm_alloc_page().
    if (reserved_base && reserved_size) {
        uint64_t reserved_start_page = reserved_base / PAGE_SIZE;
        uint64_t reserved_pages = (reserved_size + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint64_t p = 0; p < reserved_pages; p++)
            bitmap_set(reserved_start_page + p);
    }

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

// pmm_alloc_page()/pmm_free_page() модифицируют общий битмап без защиты от
// повторного входа — вложенный таймерный IRQ мог вызвать pmm_alloc_page()
// повторно до того, как первый вызов успел выставить бит занятости, и оба
// возвращали один физический адрес двум разным владельцам (наблюдалось на
// практике). Простой cli/sti вокруг критической секции это исключает.
static inline uint64_t pmm_lock(void) {
    uint64_t flags;
    asm volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    asm volatile("cli");
    return flags;
}

static inline void pmm_unlock(uint64_t flags) {
    asm volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

uint64_t pmm_alloc_page(void) {
    uint64_t flags = pmm_lock();

    for (uint64_t i = next_free_page; i < total_pages; i++) {

        if (!bitmap_test(i)) {

            bitmap_set(i);
            used_pages++;

            next_free_page = i + 1;

            pmm_unlock(flags);
            return i * PAGE_SIZE;
        }
    }

    // Дошли до конца битмапа, не найдя свободного бита — это НЕ значит,
    // что свободных страниц нет вообще: next_free_page только растёт и
    // никогда не возвращается назад, так что все страницы, которые
    // pmm_free_page() успела освободить НИЖЕ прежнего next_free_page, этим
    // циклом просто ни разу не были осмотрены повторно. Без оборота здесь
    // любой код, который много раз подряд что-то выделяет и тут же
    // освобождает (классический случай — fork()+exec() в шелле: глубокое
    // клонирование адресного пространства при fork() и его же освобождение
    // при exec()), раз за разом толкает next_free_page вперёд и никогда не
    // возвращает уже свободную память обратно в оборот — пока watermark не
    // упрётся в total_pages и pmm_alloc_page() не начнёт молча возвращать 0
    // при ещё половине физической памяти, реально свободной. Один
    // дополнительный проход по "хвосту" ниже прежнего watermark (начиная с
    // 512 — страницы 0..511 зарезервированы навсегда, см. pmm_init())
    // решает это раз и навсегда.
    for (uint64_t i = 512; i < next_free_page; i++) {

        if (!bitmap_test(i)) {

            bitmap_set(i);
            used_pages++;

            next_free_page = i + 1;

            pmm_unlock(flags);
            return i * PAGE_SIZE;
        }
    }

    pmm_unlock(flags);
    return 0;
}

// Линейный поиск count подряд идущих свободных бит начиная с самого начала
// битмапа (а не next_free_page, как в pmm_alloc_page()) — эта функция
// вызывается редко (один раз при инициализации устройства), а не на горячем
// пути, так что простота поиска важнее его скорости. При успехе выставляет
// все count бит одним проходом; при неудаче не трогает битмап вообще.
uint64_t pmm_alloc_contiguous_pages(uint32_t count) {
    if (count == 0) return 0;

    uint64_t flags = pmm_lock();

    uint64_t run_start = 0;
    uint64_t run_len = 0;

    for (uint64_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            if (run_len == 0) run_start = i;
            run_len++;
            if (run_len == count) {
                for (uint64_t p = run_start; p < run_start + count; p++) {
                    bitmap_set(p);
                }
                used_pages += count;
                pmm_unlock(flags);
                return run_start * PAGE_SIZE;
            }
        } else {
            run_len = 0;
        }
    }

    pmm_unlock(flags);
    return 0;
}

void pmm_free_page(uint64_t phys) {
    uint64_t page = phys / PAGE_SIZE;
    if (page >= total_pages) return;

    uint64_t flags = pmm_lock();
    // Страховка от двойного free(): bitmap_clear() на уже свободном бите
    // сама по себе безвредна (идемпотентна), но used_pages-- БЕЗ этой
    // проверки всё равно продолжал бы уменьшаться при каждом повторном
    // вызове — счётчик расходился бы с реальным состоянием битмапа (нашлось
    // при отладке free_user_address_space()/clone_address_space_deep():
    // настоящий баг уже исправлен, но дешёвая защита от будущих таких же
    // ошибок того стоит — иначе следующий подобный баг снова будет не
    // диагностировать по used_pages, а маскироваться под "ещё полно
    // свободной памяти", пока pmm_alloc_page() не начнёт возвращать 0 на
    // самом деле пустом битмапе).
    if (!bitmap_test(page)) {
        pmm_unlock(flags);
        return;
    }
    bitmap_clear(page);
    used_pages--;
    pmm_unlock(flags);
}

uint64_t pmm_get_total_pages(void) {
    return total_pages;
}

uint64_t pmm_get_used_pages(void) {
    return used_pages;
}