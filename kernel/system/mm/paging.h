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
// Бит 9 — свободный для ОС (аппаратура его игнорирует у обычных, не-huge
// present-записей). Ставится ТОЛЬКО на 4KB-записи, появившиеся при
// расщеплении identity-huge-страницы (map_page()/map_page_in_pml4(),
// paging.c) и ещё не перезаписанные под конкретный адрес вызывающего —
// т.е. "эта страница всё ещё физически делится со всей системой, а не
// принадлежит текущему адресному пространству". free_user_address_space()
// проверяет именно этот бит, чтобы не освобождать чужую (общую) физическую
// память — см. подробный комментарий там же про то, почему раньше
// использовалось шаткое сравнение leaf_phys==leaf_virt и к чему это
// привело. clone_address_space_deep() (process.c) обязан СНИМАТЬ этот бит
// у любой копии, которую реально продублировал (новый физический кадр —
// это уже не общая память, а собственность ребёнка).
#define PAGE_IDENTITY_SHARED 0x200
// Бит 10 — тоже свободный для ОС. v0.8-мост, пункт 4 (MAP_SHARED): страница
// принадлежит зарегистрированной в shm.c общей области (а не личной копии
// процесса) — clone_address_space_deep() (process.c) при встрече этого бита
// АЛИАСИТ тот же физический кадр вместо копирования (shm_add_ref()), а
// free_user_address_space()/unmap_page() (ниже в этом файле) освобождают
// его через shm_release() (которая реально зовёт pmm_free_page() только
// когда последний процесс отпустил область), а не напрямую. Тот же
// принцип, что и у PAGE_IDENTITY_SHARED выше — отличие в том, ЧТО именно
// общее: там общий с ядром identity-кадр, здесь общая с другим
// пользовательским процессом mmap-область.
#define PAGE_MMAP_SHARED      0x400
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

// Раньше физическую память трогали напрямую через (T*)phys, полагаясь на
// identity map PML4[0] — но ELF-загрузка процесса перезаписывает часть этого
// диапазона своими (часто read-only) страницами, и такой указатель начинал
// целить в чужую память. physmap — отдельный, никогда не переиспользуемый
// диапазон под ВСЮ RAM, поэтому phys_to_virt() всегда даёт рабочий указатель.
static inline void* phys_to_virt(uint64_t phys) {
    return (void*)(KERNEL_PHYSMAP_BASE + phys);
}

// PML4-индекс 320 — свободный (256=physmap выше, 272=kernel stacks в
// process.h, 288=kernel heap в heap.h) канонический диапазон, зарезервированный
// под ЯВНОЕ отображение MMIO физических адресов устройств (PCI BAR'ов и
// т.п.), которые НЕ являются RAM и поэтому НЕ покрываются physmap'ом (тот
// отображает только RAM, отчитанную UEFI). Первый потребитель — xHCI-драйвер.
#define KERNEL_MMIO_BASE 0xFFFFA00000000000ULL

// Отображает [phys, phys+size) как некэшируемый MMIO-регион где-то внутри
// KERNEL_MMIO_BASE и возвращает виртуальный указатель на НАЧАЛО запроса
// (т.е. уже со смещением внутри первой страницы, если phys не выровнен).
// ОБЩИЙ бамп-аллокатор для ВСЕХ MMIO-потребителей (xHCI, AHCI, ...) — v0.9,
// фаза 1: раньше это была ЧАСТНАЯ статическая переменная внутри xhci.c,
// заведённая под единственного на тот момент потребителя; второй драйвер со
// своим собственным bump-указателем, по тому же соглашению заново
// стартующим с KERNEL_MMIO_BASE, перезаписал бы отображение первого —
// map_page() молча меняет физический адрес для уже занятых им виртуальных
// страниц, не ошибка, просто тихая порча чужого MMIO-окна. Реализация в
// paging.c; NULL при нехватке памяти под промежуточные таблицы страниц.
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

// Даёт новому адресному пространству СОБСТВЕННУЮ (не общую с остальными
// процессами) копию PDPT+PD, покрывающих identity map нижних физических
// гигабайт (PML4[0], см. paging_init()). Без этого все процессы делят один
// и тот же физический PDPT/PD, и map_page_in_pml4()/map_page(), расщепляя
// huge-страницу под пользовательский код (например, ELF по 0x400000),
// портят identity map сразу для всей системы. См. подробный комментарий у
// реализации в paging.c.
void clone_low_identity_map(uint64_t dest_pml4_phys);

// Полностью освобождает пользовательскую половину (PML4[0..255]) адресного
// пространства pml4_phys — все промежуточные PDPT/PD/PT-страницы и все
// присутствующие листовые физические страницы, включая сам PML4. Kernel
// space (256-511, общий/синхронизированный через sync_kernel_mappings()) не
// трогает. См. подробный комментарий у реализации в paging.c — единственная
// функция разбора PML4 целиком, использует те же соглашения обхода/масок,
// что и clone_address_space_deep() в process.c.
void free_user_address_space(uint64_t pml4_phys);