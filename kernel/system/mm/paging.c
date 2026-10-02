// paging.c
#include "paging.h"
#include "pmm.h"
#include "bootinfo.h"
#include "lib/stddef.h"
#include "lib/types.h"
#include "log.h"

typedef uint64_t pt_entry_t;

// Тот же код типа памяти, что и в pmm.c (EFI_MEMORY_DESCRIPTOR.Type) — см.
// комментарий у его использования ниже, в paging_init().
#define EfiConventionalMemory 7

// Указатели на таблицы страниц ядра
static pt_entry_t *kernel_pml4 = NULL;
static pt_entry_t *kernel_pdpt = NULL;

#define PML4_INDEX(v)   (((v) >> 39) & 0x1FF)
#define PDPT_INDEX(v)   (((v) >> 30) & 0x1FF)
#define PD_INDEX(v)     (((v) >> 21) & 0x1FF)
#define PT_INDEX(v)     (((v) >> 12) & 0x1FF)

static inline uint64_t paddr_to_entry(uint64_t phys, uint64_t flags) {
    return (phys & 0x000FFFFFFFFFF000ULL) | (flags & 0xFFF);
}

static void invlpg(uint64_t addr) {
    asm volatile ("invlpg (%0)" : : "r" (addr) : "memory");
}

uint64_t get_current_pml4(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

// parent — виртуальный (через phys_to_virt()) указатель на родительскую
// таблицу; возвращает такой же виртуальный указатель на дочернюю. Сами
// записи остаются физическими адресами, как и положено записям таблиц
// страниц. x86-64 берёт U/S-бит по всем уровням трансляции сразу — если
// хоть один уровень supervisor-only, весь адрес считается supervisor-only
// независимо от листового PTE. Поэтому промежуточные таблицы здесь создаются
// с PAGE_USER: реальное разрешение всё равно проверяется по флагам
// листового PTE, так что разрешительные промежуточные записи ничего лишнего
// не открывают, а их отсутствие сломало бы любой ring3-доступ к памяти.
static pt_entry_t* get_or_create_table(pt_entry_t *parent, uint64_t index, int create) {
    if (!(parent[index] & PAGE_PRESENT)) {
        if (!create) return NULL;

        uint64_t phys = pmm_alloc_page();
        if (!phys) return NULL;

        parent[index] = paddr_to_entry(phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);

        pt_entry_t *table = (pt_entry_t*)phys_to_virt(phys);
        for (int i = 0; i < 512; i++) table[i] = 0;

        return table;
    } else {
        parent[index] |= PAGE_USER;
        return (pt_entry_t*)phys_to_virt(parent[index] & 0x000FFFFFFFFFF000ULL);
    }
}

// Строит по одному PD на гигабайт с huge-страницами, identity-отображающими
// физическую память 0..mem_gb*1GB. Вызывается дважды: для низкой (virt==phys)
// карты PML4[0] и для отдельного physmap (PML4[PHYSMAP_PML4_INDEX]). CR3 ещё
// не переключён на kernel_pml4, поэтому raw-указатели тут безопасны.
static void build_identity_pdpt(pt_entry_t *pdpt, uint64_t mem_gb) {
    for (uint64_t gb = 0; gb < mem_gb; gb++) {
        uint64_t pd_phys = pmm_alloc_page();
        if (!pd_phys) {
            LOG_DONE_FAIL("Paging: cannot allocate PD");
            while (1) __asm__("hlt");
        }

        pt_entry_t *pd = (pt_entry_t*)pd_phys;
        for (int i = 0; i < 512; i++) pd[i] = 0;

        pdpt[gb] = paddr_to_entry(pd_phys, PAGE_PRESENT | PAGE_WRITE);

        for (int i = 0; i < 512; i++) {
            uint64_t phys = (gb << 30) + (i << 21);
            pd[i] = paddr_to_entry(phys, PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE);
        }
    }
}

void paging_init(BootInfo* bi) {
    LOG_PENDING("Initializing paging...");

    static pt_entry_t pml4_table[512] __attribute__((aligned(4096)));
    static pt_entry_t pdpt_table[512] __attribute__((aligned(4096)));
    static pt_entry_t physmap_pdpt[512] __attribute__((aligned(4096)));

    kernel_pml4 = pml4_table;
    kernel_pdpt = pdpt_table;

    for (int i = 0; i < 512; i++) kernel_pml4[i] = 0;
    for (int i = 0; i < 512; i++) kernel_pdpt[i] = 0;
    for (int i = 0; i < 512; i++) physmap_pdpt[i] = 0;

    // Identity mapping для нижней половины (первые 512 GB)
    kernel_pml4[0] = paddr_to_entry((uint64_t)kernel_pdpt, PAGE_PRESENT | PAGE_WRITE);

    // Отдельная, никогда не расщепляемая ELF-загрузкой карта всей RAM в
    // kernel space — см. phys_to_virt() в paging.h.
    kernel_pml4[PHYSMAP_PML4_INDEX] = paddr_to_entry((uint64_t)physmap_pdpt, PAGE_PRESENT | PAGE_WRITE);

    uint8_t *map = (uint8_t*)bi->MemoryMap;
    uint64_t desc_size = bi->MemoryMapDescriptorSize;
    uint64_t desc_count = bi->MemoryMapSize / desc_size;
    uint64_t max_phys = 0;

    // ВАЖНО: только EfiConventionalMemory (как и pmm_init(), pmm.c — тот же
    // самый фильтр) — без него этот скан цеплял и высокие MMIO/reserved
    // регионы из карты памяти (PCI64-окно OVMF, ACPI-таблицы и т.п.,
    // которые реальным RAM не являются), раздувая max_phys до значений в
    // десятки-сотни GB при 256MB РЕАЛЬНОЙ памяти у VM. mem_gb упирался в
    // потолок (512) и build_identity_pdpt() строила identity-карту на
    // 512 GB вместо ~1 — а clone_low_identity_map() (process_create(),
    // process.c) и clone_address_space_deep() (process_fork()) потом на
    // КАЖДОМ создании/форке процесса честно копировали все эти 512
    // "призрачных" GB-регионов (ни один из которых не соответствует
    // настоящей памяти), выделяя и копируя по странице на каждый —
    // ~500-1500 физических страниц churn'а за один fork()+exec(), что и
    // приводило к исчерпанию PMM после полутора-двух сотен подряд идущих
    // команд шелла, несмотря на то что сам churn был формально
    // сбалансирован (alloc и free парно).
    for (uint64_t i = 0; i < desc_count; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR*)(map + i * desc_size);
        if (d->Type != EfiConventionalMemory) continue;
        uint64_t end = d->PhysicalStart + d->NumberOfPages * PAGE_SIZE;
        if (end > max_phys) max_phys = end;
    }

    // Фреймбуфер (bi->FrameBufferBase) консоль (drivers/console/console.c,
    // "framebuffer = (uint32_t*)bi->FrameBufferBase") адресует НАПРЯМУЮ как
    // physical==virtual указатель — а GOP-фреймбуферы живут далеко ЗА
    // пределами настоящей RAM (у QEMU/OVMF обычно в районе нескольких GB, в
    // отдельном MMIO-окне). EfiConventionalMemory-фильтр выше корректно его
    // не считает (это не RAM), но identity-карта ниже обязана его всё равно
    // покрывать — иначе первая же запись в консоль ПОСЛЕ переключения CR3
    // page-fault'ится в пустоту (строка framebuffer[...] = ...), потому что
    // новая, уже правильно маленькая (по размеру настоящей RAM) identity-карта
    // больше не простирается так далеко, как раздутая прежняя (см. основной
    // комментарий выше про mem_gb=512). Чиним явным расширением max_phys под
    // ОДИН конкретный дополнительный регион, а не возвратом к "взять верхнюю
    // границу вообще всей карты памяти".
    uint64_t fb_end = bi->FrameBufferBase + bi->FrameBufferSize;
    if (fb_end > max_phys) max_phys = fb_end;

    uint64_t mem_gb = (max_phys + (1ULL << 30) - 1) >> 30;
    if (mem_gb > 512) mem_gb = 512;

    build_identity_pdpt(kernel_pdpt, mem_gb);
    build_identity_pdpt(physmap_pdpt, mem_gb);

    asm volatile ("mov %0, %%cr3" : : "r" ((uint64_t)kernel_pml4) : "memory");

    LOG_DONE_OK("Paging: identity mapped up to 0x%lx (+ kernel physmap)", max_phys);
}

int map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    pt_entry_t *pml4 = (pt_entry_t*)phys_to_virt(get_current_pml4());
    if (!pml4) return -1;

    pt_entry_t *pdpt_table = get_or_create_table(pml4, PML4_INDEX(virt), 1);
    if (!pdpt_table) return -1;

    pt_entry_t *pd_table = get_or_create_table(pdpt_table, PDPT_INDEX(virt), 1);
    if (!pd_table) return -1;

    uint64_t pd_idx = PD_INDEX(virt);

    // If we hit a huge page, split it into 4KB pages
    if ((pd_table[pd_idx] & PAGE_PRESENT) && (pd_table[pd_idx] & PAGE_HUGE)) {
        uint64_t huge_entry = pd_table[pd_idx];
        uint64_t phys_base = huge_entry & 0x000FFFFFFFFFF000ULL;
        uint64_t pde_flags = (huge_entry & 0xFFF) & ~PAGE_HUGE;   // keep all flags except huge

        uint64_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return -1;

        // Все 512 записей здесь — ещё нерасщеплённая identity-память (та же
        // физическая страница, что виртуальный адрес), пока ниже под
        // PT_INDEX(virt) одну из них не перезапишут под конкретный запрос
        // вызывающего — помечаем ИХ ВСЕ как общие, чтобы
        // free_user_address_space() знал, что их нельзя отдавать обратно в
        // pmm (см. PAGE_IDENTITY_SHARED, paging.h).
        pt_entry_t *pt = (pt_entry_t*)phys_to_virt(pt_phys);
        for (int i = 0; i < 512; i++) {
            pt[i] = paddr_to_entry(phys_base + i * PAGE_SIZE, pde_flags | PAGE_IDENTITY_SHARED);
        }

        pd_table[pd_idx] = paddr_to_entry(pt_phys, pde_flags);
        // flush the huge TLB entry
        invlpg(virt & ~0x1FFFFFULL);
    }

    pt_entry_t *pt_table = get_or_create_table(pd_table, pd_idx, 1);
    if (!pt_table) return -1;

    pt_table[PT_INDEX(virt)] = (phys & 0x000FFFFFFFFFF000ULL) | (flags & 0xFFF) | PAGE_PRESENT;
    invlpg(virt);
    return 0;
}

// Функция для маппинга с указанным PML4 (для процессов)
int map_page_in_pml4(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    pt_entry_t *pml4 = (pt_entry_t*)phys_to_virt(pml4_phys);
    if (!pml4) return -1;

    pt_entry_t *pdpt_table = get_or_create_table(pml4, PML4_INDEX(virt), 1);
    if (!pdpt_table) return -1;

    pt_entry_t *pd_table = get_or_create_table(pdpt_table, PDPT_INDEX(virt), 1);
    if (!pd_table) return -1;

    uint64_t pd_idx = PD_INDEX(virt);

    if ((pd_table[pd_idx] & PAGE_PRESENT) && (pd_table[pd_idx] & PAGE_HUGE)) {
        uint64_t huge_entry = pd_table[pd_idx];
        uint64_t phys_base = huge_entry & 0x000FFFFFFFFFF000ULL;
        uint64_t pde_flags = (huge_entry & 0xFFF) & ~PAGE_HUGE;

        uint64_t pt_phys = pmm_alloc_page();
        if (!pt_phys) return -1;

        // См. тот же комментарий в map_page() выше — PAGE_IDENTITY_SHARED
        // на все 512, пока одну из них не перезапишут под PT_INDEX(virt).
        pt_entry_t *pt = (pt_entry_t*)phys_to_virt(pt_phys);
        for (int i = 0; i < 512; i++) {
            pt[i] = paddr_to_entry(phys_base + i * PAGE_SIZE, pde_flags | PAGE_IDENTITY_SHARED);
        }

        pd_table[pd_idx] = paddr_to_entry(pt_phys, pde_flags);
        invlpg(virt & ~0x1FFFFFULL);
    }

    pt_entry_t *pt_table = get_or_create_table(pd_table, pd_idx, 1);
    if (!pt_table) return -1;

    pt_table[PT_INDEX(virt)] = (phys & 0x000FFFFFFFFFF000ULL) | (flags & 0xFFF) | PAGE_PRESENT;
    return 0;
}

void unmap_page(uint64_t virt) {
    pt_entry_t *pml4 = (pt_entry_t*)phys_to_virt(get_current_pml4());
    if (!pml4) return;

    pt_entry_t *pml4e = &pml4[PML4_INDEX(virt)];
    if (!(*pml4e & PAGE_PRESENT)) return;

    pt_entry_t *pdpt_table = (pt_entry_t*)phys_to_virt(*pml4e & 0x000FFFFFFFFFF000ULL);
    pt_entry_t *pdpte = &pdpt_table[PDPT_INDEX(virt)];
    if (!(*pdpte & PAGE_PRESENT)) return;

    pt_entry_t *pd_table = (pt_entry_t*)phys_to_virt(*pdpte & 0x000FFFFFFFFFF000ULL);
    pt_entry_t *pde = &pd_table[PD_INDEX(virt)];
    if (!(*pde & PAGE_PRESENT)) return;

    // A huge page cannot be partially unmapped; bail out safely
    if (*pde & PAGE_HUGE) {
        return;
    }

    pt_entry_t *pt_table = (pt_entry_t*)phys_to_virt(*pde & 0x000FFFFFFFFFF000ULL);
    pt_entry_t *pte = &pt_table[PT_INDEX(virt)];
    
    uint64_t phys = *pte & 0x000FFFFFFFFFF000ULL;
    if (phys) {
        pmm_free_page(phys);
    }
    
    *pte = 0;
    invlpg(virt);
}

// Как get_physical_address(), но работает с ЛЮБЫМ адресным пространством
// по его физическому PML4, а не только с текущим (активным по CR3) —
// через identity mapping, без переключения CR3.
uint64_t get_physical_address_in_pml4(uint64_t pml4_phys, uint64_t virt) {
    pt_entry_t *pml4 = (pt_entry_t*)phys_to_virt(pml4_phys);
    if (!pml4) return 0;

    pt_entry_t *pml4e = &pml4[PML4_INDEX(virt)];
    if (!(*pml4e & PAGE_PRESENT)) return 0;

    pt_entry_t *pdpt_table = (pt_entry_t*)phys_to_virt(*pml4e & 0x000FFFFFFFFFF000ULL);
    pt_entry_t *pdpte = &pdpt_table[PDPT_INDEX(virt)];
    if (!(*pdpte & PAGE_PRESENT)) return 0;

    pt_entry_t *pd_table = (pt_entry_t*)phys_to_virt(*pdpte & 0x000FFFFFFFFFF000ULL);
    pt_entry_t *pde = &pd_table[PD_INDEX(virt)];
    if (!(*pde & PAGE_PRESENT)) return 0;

    if (*pde & PAGE_HUGE) {
        return (*pde & 0x000FFFFFFFFFF000ULL) + (virt & 0x1FFFFF);
    } else {
        pt_entry_t *pt_table = (pt_entry_t*)phys_to_virt(*pde & 0x000FFFFFFFFFF000ULL);
        pt_entry_t *pte = &pt_table[PT_INDEX(virt)];
        if (!(*pte & PAGE_PRESENT)) return 0;
        return (*pte & 0x000FFFFFFFFFF000ULL) + (virt & 0xFFF);
    }
}

// Проверяет, что 4KB-страница, содержащая virt, отображена в pml4_phys и
// доступна из ring3: PAGE_USER должен стоять на каждом уровне трансляции —
// "низкий/канонический адрес" сам по себе ничего не доказывает, код ядра
// (без PAGE_USER) и пользовательская память лежат в одном диапазоне.
// need_write дополнительно требует PAGE_WRITE на листовой записи (PDE для
// huge-страницы, иначе PTE — расщепление huge-страницы по частям при первом
// обращении означает, что разные 4KB одного 2MB-региона могут иметь разные
// флаги).
int is_user_accessible(uint64_t pml4_phys, uint64_t virt, int need_write) {
    pt_entry_t *pml4 = (pt_entry_t*)phys_to_virt(pml4_phys);
    if (!pml4) return 0;

    pt_entry_t pml4e = pml4[PML4_INDEX(virt)];
    if (!(pml4e & PAGE_PRESENT) || !(pml4e & PAGE_USER)) return 0;

    pt_entry_t *pdpt = (pt_entry_t*)phys_to_virt(pml4e & 0x000FFFFFFFFFF000ULL);
    pt_entry_t pdpte = pdpt[PDPT_INDEX(virt)];
    if (!(pdpte & PAGE_PRESENT) || !(pdpte & PAGE_USER)) return 0;

    pt_entry_t *pd = (pt_entry_t*)phys_to_virt(pdpte & 0x000FFFFFFFFFF000ULL);
    pt_entry_t pde = pd[PD_INDEX(virt)];
    if (!(pde & PAGE_PRESENT)) return 0;

    if (pde & PAGE_HUGE) {
        if (!(pde & PAGE_USER)) return 0;
        if (need_write && !(pde & PAGE_WRITE)) return 0;
        return 1;
    }
    if (!(pde & PAGE_USER)) return 0;

    pt_entry_t *pt = (pt_entry_t*)phys_to_virt(pde & 0x000FFFFFFFFFF000ULL);
    pt_entry_t pte = pt[PT_INDEX(virt)];
    if (!(pte & PAGE_PRESENT) || !(pte & PAGE_USER)) return 0;
    if (need_write && !(pte & PAGE_WRITE)) return 0;

    return 1;
}

// То же самое для целого диапазона [addr, addr+len) — проверяет КАЖДУЮ
// затронутую 4KB-страницу отдельно, а не только первую/последнюю (см.
// комментарий у is_user_accessible() про расщепление huge-страниц).
// len==0 тривиально валиден (нечего проверять).
int is_user_range_valid(uint64_t pml4_phys, uint64_t addr, uint64_t len, int need_write) {
    if (len == 0) return 1;

    uint64_t end = addr + len - 1;
    if (end < addr) return 0;   // переполнение addr+len

    uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last_page = end & ~(uint64_t)(PAGE_SIZE - 1);
    for (;;) {
        if (!is_user_accessible(pml4_phys, page, need_write)) return 0;
        if (page == last_page) break;
        page += PAGE_SIZE;
    }
    return 1;
}

uint64_t get_physical_address(uint64_t virt) {
    return get_physical_address_in_pml4(get_current_pml4(), virt);
}

// Раньше PML4[0] всех процессов указывал на ОДИН общий kernel_pdpt/PD — когда
// ELF одного процесса расщеплял huge-страницу под свой код, это меняло
// identity map сразу для всей системы, и другие процессы падали в page fault
// на уже отданных под что-то другое физических страницах. Даём каждому
// процессу приватную копию PDPT+PD (leaf-записи huge-страниц — по значению).
void clone_low_identity_map(uint64_t dest_pml4_phys) {
    pt_entry_t *dest_pml4 = (pt_entry_t*)phys_to_virt(dest_pml4_phys);

    uint64_t new_pdpt_phys = pmm_alloc_page();
    if (!new_pdpt_phys) return;

    pt_entry_t *new_pdpt = (pt_entry_t*)phys_to_virt(new_pdpt_phys);
    for (int i = 0; i < 512; i++) new_pdpt[i] = 0;

    for (int gb = 0; gb < 512; gb++) {
        if (!(kernel_pdpt[gb] & PAGE_PRESENT)) continue;

        pt_entry_t *src_pd = (pt_entry_t*)phys_to_virt(kernel_pdpt[gb] & 0x000FFFFFFFFFF000ULL);

        uint64_t new_pd_phys = pmm_alloc_page();
        if (!new_pd_phys) continue;

        pt_entry_t *new_pd = (pt_entry_t*)phys_to_virt(new_pd_phys);
        for (int i = 0; i < 512; i++) new_pd[i] = src_pd[i];

        new_pdpt[gb] = paddr_to_entry(new_pd_phys, kernel_pdpt[gb] & 0xFFF);
    }

    dest_pml4[0] = paddr_to_entry(new_pdpt_phys, PAGE_PRESENT | PAGE_WRITE);
}

// Единственная функция, разбирающая PML4 целиком — обходит пользовательскую
// половину (индексы 0-255) PDPT->PD->PT, освобождает каждую присутствующую
// физическую страницу (листовую и промежуточные таблицы), затем сам PML4.
// Kernel space (256-511) не трогает — общая между процессами.
//
// PML4[0] — особый случай: рядом с настоящей памятью процесса там может
// лежать нерасщеплённая huge-страница из clone_low_identity_map() (общая
// identity-копия ядра, освобождать нельзя — см. PAGE_HUGE-ветку ниже), а
// после расщепления такой huge-страницы под конкретный адрес процесса
// соответствующая PT содержит смесь: одна запись — реальная страница
// процесса, остальные 511 — нетронутые identity-копии (PAGE_IDENTITY_SHARED,
// проставляется в map_page()/map_page_in_pml4() при расщеплении — см.
// paging.h). Раньше вместо явного бита использовалось сравнение
// leaf_phys==leaf_virt ("у нетронутой identity-записи физический адрес
// совпадает с виртуальным, у настоящей pmm_alloc_page()-страницы это
// совпадение практически невозможно") — но это было верно только пока
// pmm_alloc_page() монотонно шёл вперёд и никогда не выдавал НИЗКИЕ
// физические адреса повторно. Как только появился оборот битмапа (см.
// pmm_alloc_page(), pmm.c) — клонированная fork()'ом копия такой
// "identity" PT (clone_address_space_deep(), process.c, копирует ВСЕ 512
// записей в НОВЫЕ физические страницы, включая эти 511 "обёрнутых") вполне
// может получить от pmm_alloc_page() низкий физический адрес, случайно
// совпадающий с виртуальным — и тогда эта СОБСТВЕННАЯ, уже ни с кем не
// общая страница ребёнка никогда не освобождалась бы: постоянная утечка
// ~2MB на каждый fork()+exec(), в точности то, что и наблюдалось. Явный бит
// не зависит от numeric coincidence вообще.
void free_user_address_space(uint64_t pml4_phys) {
    pt_entry_t *pml4 = (pt_entry_t*)phys_to_virt(pml4_phys);
    if (!pml4) return;

    for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
        if (!(pml4[pml4_idx] & PAGE_PRESENT)) continue;
        uint64_t pdpt_phys = pml4[pml4_idx] & 0x000FFFFFFFFFF000ULL;
        pt_entry_t *pdpt = (pt_entry_t*)phys_to_virt(pdpt_phys);

        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) continue;
            uint64_t pd_phys = pdpt[pdpt_idx] & 0x000FFFFFFFFFF000ULL;
            pt_entry_t *pd = (pt_entry_t*)phys_to_virt(pd_phys);

            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                if (!(pd[pd_idx] & PAGE_PRESENT)) continue;

                if (pd[pd_idx] & PAGE_HUGE) {
                    // Нерасщеплённая huge-страница в пользовательской
                    // половине встречается только в PML4[0] и всегда это
                    // общая identity-карта ядра (см. clone_low_identity_map())
                    // — не память процесса. Физическую страницу не трогаем.
                    continue;
                }

                uint64_t pt_phys = pd[pd_idx] & 0x000FFFFFFFFFF000ULL;
                pt_entry_t *pt = (pt_entry_t*)phys_to_virt(pt_phys);

                for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                    if (!(pt[pt_idx] & PAGE_PRESENT)) continue;
                    if (pt[pt_idx] & PAGE_IDENTITY_SHARED) continue;

                    uint64_t leaf_phys = pt[pt_idx] & 0x000FFFFFFFFFF000ULL;
                    pmm_free_page(leaf_phys);
                }
                pmm_free_page(pt_phys);
            }
            pmm_free_page(pd_phys);
        }
        pmm_free_page(pdpt_phys);
    }
    pmm_free_page(pml4_phys);
}

// Синхронизация kernel space записей между PML4 (для процессов)
void sync_kernel_mappings(uint64_t dest_pml4_phys, uint64_t src_pml4_phys) {
    pt_entry_t *dest_pml4 = (pt_entry_t*)phys_to_virt(dest_pml4_phys);
    pt_entry_t *src_pml4 = (pt_entry_t*)phys_to_virt(src_pml4_phys);

    for (int i = 256; i < 512; i++) {  // Только kernel space (верхняя половина)
        if (src_pml4[i] & PAGE_PRESENT) {
            uint64_t table_phys = src_pml4[i] & 0x000FFFFFFFFFF000ULL;
            // Копируем всю таблицу PDPT для этого индекса
            pt_entry_t *src_table = (pt_entry_t*)phys_to_virt(table_phys);

            if (!(dest_pml4[i] & PAGE_PRESENT)) {
                // Выделяем новую PDPT для destination
                uint64_t new_table_phys = pmm_alloc_page();
                if (new_table_phys) {
                    dest_pml4[i] = (new_table_phys & ~0xFFF) | (src_pml4[i] & 0xFFF);
                    pt_entry_t *dest_table = (pt_entry_t*)phys_to_virt(new_table_phys);
                    // Копируем все записи
                    for (int j = 0; j < 512; j++) {
                        dest_table[j] = src_table[j];
                    }
                }
            } else {
                // Обновляем существующую таблицу
                pt_entry_t *dest_table = (pt_entry_t*)phys_to_virt(dest_pml4[i] & 0x000FFFFFFFFFF000ULL);
                for (int j = 0; j < 512; j++) {
                    dest_table[j] = src_table[j];
                }
            }
        }
    }
}