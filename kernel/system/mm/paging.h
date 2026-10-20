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
// Бит 9 — свободный для ОС, ставится ТОЛЬКО на 4KB-записи, появившиеся при
// расщеплении identity-huge-страницы (map_page()/map_page_in_pml4()) и ещё
// не перезаписанные под конкретный адрес — "страница всё ещё общая со всей
// системой, не принадлежит текущему адресному пространству".
// free_user_address_space() по этому биту не освобождает чужую физическую
// память; clone_address_space_deep() (process.c) обязан снимать бит у любой
// реально продублированной копии (новый физический кадр — уже не общий).
#define PAGE_IDENTITY_SHARED 0x200
// Бит 10 — тоже свободный для ОС: страница принадлежит общей области,
// зарегистрированной в shm.c (MAP_SHARED), а не личной копии процесса.
// clone_address_space_deep() (process.c) при этом бите алиасит тот же
// физический кадр (shm_add_ref()) вместо копирования; free_user_address_space()/
// unmap_page() освобождают его через shm_release() (реальный pmm_free_page()
// только когда область отпустит последний процесс).
#define PAGE_MMAP_SHARED      0x400
// Бит 11 — тоже свободный для ОС: лист отображает физический адрес, которым
// pmm НЕ владеет (PCI BAR устройства, отданный userspace-драйверу через
// SYS_MMAP_MMIO, driver.h) — это не страница RAM вообще, а регистры
// устройства. unmap_page()/free_user_address_space() обязаны пропускать
// pmm_free_page() для такого листа: вызов на MMIO-адрес либо не найдёт его
// в битовой карте pmm (ушёл бы в никуда), либо, если адрес случайно
// попадает в её диапазон, испортит битовую карту и в итоге отдаст физическую
// память устройства какому-то совсем другому процессу.
#define PAGE_MMIO_NOFREE      0x800
#define PAGE_NX              (1ULL << 63)

// PML4-индекс отдельного, НИКОГДА не подверженного хайджеку, kernel-space
// отображения всей физической памяти (physmap) — см. phys_to_virt() ниже.
#define PHYSMAP_PML4_INDEX 256
// ВАЖНО: НЕ (PHYSMAP_PML4_INDEX << 39) — даёт 0x0000800000000000,
// неканонический адрес. PML4-индекс >= 256 означает бит 47 = 1, а для
// канонической формы биты 63-48 должны быть SIGN-EXTENDED в единицы (как и
// KERNEL_STACK_AREA_START/KERNEL_HEAP_START в process.h) — отсюда
// 0xFFFF800000000000.
#define KERNEL_PHYSMAP_BASE 0xFFFF800000000000ULL

// Прямой доступ через (T*)phys (в расчёте на identity map PML4[0]) ломается,
// когда ELF-загрузка процесса перезаписывает часть этого диапазона своими
// страницами. physmap — отдельный, никогда не переиспользуемый диапазон под
// ВСЮ RAM, поэтому phys_to_virt() всегда даёт рабочий указатель.
static inline void* phys_to_virt(uint64_t phys) {
    return (void*)(KERNEL_PHYSMAP_BASE + phys);
}

// PML4-индекс 320 — свободный канонический диапазон (256=physmap, 272=kernel
// stacks, 288=kernel heap) под явное отображение MMIO физических адресов
// устройств (PCI BAR и т.п.), не покрываемых physmap (только настоящая RAM,
// отчитанная UEFI). Первый потребитель — xHCI-драйвер.
#define KERNEL_MMIO_BASE 0xFFFFA00000000000ULL

// Отображает [phys, phys+size) как некэшируемый MMIO-регион внутри
// KERNEL_MMIO_BASE, возвращает виртуальный указатель на начало запроса (со
// смещением внутри первой страницы, если phys не выровнен). ОБЩИЙ
// бамп-аллокатор для всех MMIO-потребителей (xHCI, AHCI, ...): отдельный
// bump-указатель на драйвер, стартующий с того же KERNEL_MMIO_BASE, привёл бы
// к тому, что второй драйвер молча переписывает отображение первого
// (map_page() это не считает ошибкой). Реализация в paging.c; NULL при
// нехватке памяти под таблицы страниц.
void *mmio_map(uint64_t phys, uint64_t size);

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

// Проверка user-указателей, приходящих в syscall'ы (см. paging.c) — истина,
// только если ВСЕ уровни трансляции для этого адреса/диапазона имеют
// PAGE_USER (и PAGE_WRITE, если need_write).
int is_user_accessible(uint64_t pml4_phys, uint64_t virt, int need_write);
int is_user_range_valid(uint64_t pml4_phys, uint64_t addr, uint64_t len, int need_write);

// Получить физический адрес текущего PML4
uint64_t get_current_pml4(void);

// Синхронизировать kernel mappings между PML4
void sync_kernel_mappings(uint64_t dest_pml4_phys, uint64_t src_pml4_phys);

// Даёт новому адресному пространству собственную (не общую) копию PDPT+PD
// для identity map нижних физических гигабайт (PML4[0]). Без этого все
// процессы делят общий PDPT/PD, и расщепление huge-страницы одним процессом
// (map_page()/map_page_in_pml4(), например под ELF-код) портит identity map
// всем. См. реализацию в paging.c.
void clone_low_identity_map(uint64_t dest_pml4_phys);

// Полностью освобождает пользовательскую половину (PML4[0..255]):
// промежуточные PDPT/PD/PT и все присутствующие листовые физические
// страницы, включая сам PML4. Kernel space (256-511, синхронизируется через
// sync_kernel_mappings()) не трогает. См. реализацию в paging.c — те же
// соглашения обхода/масок, что у clone_address_space_deep() в process.c.
void free_user_address_space(uint64_t pml4_phys);