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

// PML4-индекс отдельного, НИКОГДА не подверженного хайджеку, kernel-space
// отображения всей физической памяти (physmap) — см. phys_to_virt() ниже.
#define PHYSMAP_PML4_INDEX 256
// ВАЖНО: НЕ (((uint64_t)PHYSMAP_PML4_INDEX) << 39) — это даёт 0x0000800000000000,
// НЕканонический адрес. Индекс PML4 >= 256 означает бит 47 виртуального
// адреса равен 1, а для канонической формы биты 63-48 обязаны быть
// SIGN-EXTENDED в единицы (как и у уже существующих KERNEL_STACK_AREA_START/
// KERNEL_HEAP_START в process.h) — отсюда 0xFFFF800000000000, а не
// 0x0000800000000000.
#define KERNEL_PHYSMAP_BASE 0xFFFF800000000000ULL

// Все места в ядре, которым нужно прочитать/записать данные ПО ФИЗИЧЕСКОМУ
// адресу как по указателю (стенки таблиц страниц процессов, элементы
// process_create()'s начального кадра стека и т.п.) — то есть буквально
// ВЕСЬ paging.c/process.c/elf.c, — раньше делали это напрямую: (T*)phys,
// полагаясь на то, что PML4[0] (низкая половина, virt==phys) у ЛЮБОГО
// активного CR3 identity-мапит всю RAM. Это работало, ПОКА в этот же самый
// диапазон (обычно 0x400000+) не подгружался ELF пользовательского
// процесса: create_address_space()/clone_low_identity_map() дают каждому
// процессу СВОЮ копию этого диапазона (не расшаренную — см. paging.c), но
// сам диапазон АДРЕСОВ у "физического указателя" и у "виртуального адреса
// загрузки ELF" СОВПАДАЕТ. Как только ELF-сегмент замаплен по 0x401000, для
// ЭТОГО КОНКРЕТНОГО процесса виртуальный адрес 0x401000 больше не означает
// "физическая страница 0x401000" — он означает "страница кода этого ELF",
// зачастую read-only. Любой код, который в этот момент (например, во время
// fork() — process_fork() выполняется ПОД CR3 самого форкающегося процесса)
// попробует через "физический указатель" записать в СЛУЧАЙНО совпавшую с
// 0x401000 физическую страницу (например, свежевыделенную pmm_alloc_page()
// под новую таблицу страниц) — упадёт в page fault: физически пишет не
// туда, куда думает, а в чужую read-only страницу кода процесса.
//
// physmap — отдельный, ВСЕГДА присутствующий (kernel space, синхронизируется
// как обычный kernel-space диапазон — см. sync_kernel_mappings()) диапазон
// адресов, в который НИКОГДА ничего, кроме самого physmap, не мапится: ни
// ELF, ни пользовательский/ring0-стек (у них у всех свои, другие базовые
// адреса). phys_to_virt(phys) — универсальный, НИКОГДА не хайджекаемый
// способ получить рабочий указатель на физическую страницу под ЛЮБЫМ
// активным CR3.
static inline void* phys_to_virt(uint64_t phys) {
    return (void*)(KERNEL_PHYSMAP_BASE + phys);
}

// Инициализация страничной адресации
void paging_init(BootInfo* bi);

// Отобразить виртуальную страницу на физический адрес
int map_page(uint64_t virt, uint64_t phys, uint64_t flags);

// Отобразить страницу с указанным PML4
int map_page_in_pml4(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);

// Снять отображение
void unmap_page(uint64_t virt);

// Получить физический адрес по виртуальному
uint64_t get_physical_address(uint64_t virt);

// То же самое, но для ЛЮБОГО адресного пространства по его физическому
// PML4 — не переключая CR3 (identity mapping).
uint64_t get_physical_address_in_pml4(uint64_t pml4_phys, uint64_t virt);

// Получить физический адрес текущего PML4
uint64_t get_current_pml4(void);

// Синхронизировать kernel mappings между PML4
void sync_kernel_mappings(uint64_t dest_pml4_phys, uint64_t src_pml4_phys);

// Даёт новому адресному пространству СОБСТВЕННУЮ (не общую с остальными
// процессами) копию PDPT+PD, покрывающих identity map нижних физических
// гигабайт (PML4[0], см. paging_init()). Без этого все процессы делят один
// и тот же физический PDPT/PD, и map_page_in_pml4()/map_page(), расщепляя
// huge-страницу под пользовательский код (например, ELF по 0x400000),
// портят identity map сразу для всей системы. См. подробный комментарий у
// реализации в paging.c.
void clone_low_identity_map(uint64_t dest_pml4_phys);