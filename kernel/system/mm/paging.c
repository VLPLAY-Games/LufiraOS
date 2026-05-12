// paging.c
#include "paging.h"
#include "pmm.h"
#include "bootinfo.h"
#include "lib/stddef.h"
#include "lib/types.h"
#include "log.h"

typedef uint64_t pt_entry_t;

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

// ВАЖНО: get_or_create_table работает с ФИЗИЧЕСКИМИ адресами таблиц
// Так как у нас identity mapping, это безопасно
static pt_entry_t* get_or_create_table(pt_entry_t *parent, uint64_t index, int create) {
    if (!(parent[index] & PAGE_PRESENT)) {
        if (!create) return NULL;
        
        uint64_t phys = pmm_alloc_page();
        if (!phys) return NULL;
        
        parent[index] = paddr_to_entry(phys, PAGE_PRESENT | PAGE_WRITE);
        
        // Обнуляем таблицу (через физический адрес, так как есть identity mapping)
        pt_entry_t *table = (pt_entry_t*)phys;
        for (int i = 0; i < 512; i++) table[i] = 0;
        
        return table;
    } else {
        return (pt_entry_t*)(parent[index] & 0x000FFFFFFFFFF000ULL);
    }
}

void paging_init(BootInfo* bi) {
    LOG_PENDING("Initializing paging...");

    static pt_entry_t pml4_table[512] __attribute__((aligned(4096)));
    static pt_entry_t pdpt_table[512] __attribute__((aligned(4096)));

    kernel_pml4 = pml4_table;
    kernel_pdpt = pdpt_table;

    for (int i = 0; i < 512; i++) kernel_pml4[i] = 0;
    for (int i = 0; i < 512; i++) kernel_pdpt[i] = 0;

    // Identity mapping для нижней половины (первые 512 GB)
    kernel_pml4[0] = paddr_to_entry((uint64_t)kernel_pdpt, PAGE_PRESENT | PAGE_WRITE);

    uint8_t *map = (uint8_t*)bi->MemoryMap;
    uint64_t desc_size = bi->MemoryMapDescriptorSize;
    uint64_t desc_count = bi->MemoryMapSize / desc_size;
    uint64_t max_phys = 0;

    for (uint64_t i = 0; i < desc_count; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR*)(map + i * desc_size);
        uint64_t end = d->PhysicalStart + d->NumberOfPages * PAGE_SIZE;
        if (end > max_phys) max_phys = end;
    }

    uint64_t mem_gb = (max_phys + (1ULL << 30) - 1) >> 30;
    if (mem_gb > 512) mem_gb = 512;

    for (uint64_t gb = 0; gb < mem_gb; gb++) {
        uint64_t pd_phys = pmm_alloc_page();
        if (!pd_phys) {
            LOG_DONE_FAIL("Paging: cannot allocate PD");
            while(1) __asm__("hlt");
        }

        pt_entry_t *pd = (pt_entry_t*)pd_phys;
        for (int i = 0; i < 512; i++) pd[i] = 0;

        kernel_pdpt[gb] = paddr_to_entry(pd_phys, PAGE_PRESENT | PAGE_WRITE);

        for (int i = 0; i < 512; i++) {
            uint64_t phys = (gb << 30) + (i << 21);
            pd[i] = paddr_to_entry(phys, PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE);
        }
    }

    asm volatile ("mov %0, %%cr3" : : "r" ((uint64_t)kernel_pml4) : "memory");

    LOG_DONE_OK("Paging: identity mapped up to 0x%lx", max_phys);
}

int map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    pt_entry_t *pml4 = (pt_entry_t*)get_current_pml4();
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

        pt_entry_t *pt = (pt_entry_t*)pt_phys;
        for (int i = 0; i < 512; i++) {
            pt[i] = paddr_to_entry(phys_base + i * PAGE_SIZE, pde_flags);
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
    pt_entry_t *pml4 = (pt_entry_t*)pml4_phys;
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

        pt_entry_t *pt = (pt_entry_t*)pt_phys;
        for (int i = 0; i < 512; i++) {
            pt[i] = paddr_to_entry(phys_base + i * PAGE_SIZE, pde_flags);
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
    pt_entry_t *pml4 = (pt_entry_t*)get_current_pml4();
    if (!pml4) return;
    
    pt_entry_t *pml4e = &pml4[PML4_INDEX(virt)];
    if (!(*pml4e & PAGE_PRESENT)) return;
    
    pt_entry_t *pdpt_table = (pt_entry_t*)(*pml4e & 0x000FFFFFFFFFF000ULL);
    pt_entry_t *pdpte = &pdpt_table[PDPT_INDEX(virt)];
    if (!(*pdpte & PAGE_PRESENT)) return;
    
    pt_entry_t *pd_table = (pt_entry_t*)(*pdpte & 0x000FFFFFFFFFF000ULL);
    pt_entry_t *pde = &pd_table[PD_INDEX(virt)];
    if (!(*pde & PAGE_PRESENT)) return;
    
    // A huge page cannot be partially unmapped; bail out safely
    if (*pde & PAGE_HUGE) {
        return;
    }
    
    pt_entry_t *pt_table = (pt_entry_t*)(*pde & 0x000FFFFFFFFFF000ULL);
    pt_entry_t *pte = &pt_table[PT_INDEX(virt)];
    
    uint64_t phys = *pte & 0x000FFFFFFFFFF000ULL;
    if (phys) {
        pmm_free_page(phys);
    }
    
    *pte = 0;
    invlpg(virt);
}

uint64_t get_physical_address(uint64_t virt) {
    pt_entry_t *pml4 = (pt_entry_t*)get_current_pml4();
    if (!pml4) return 0;
    
    pt_entry_t *pml4e = &pml4[PML4_INDEX(virt)];
    if (!(*pml4e & PAGE_PRESENT)) return 0;
    
    pt_entry_t *pdpt_table = (pt_entry_t*)(*pml4e & 0x000FFFFFFFFFF000ULL);
    pt_entry_t *pdpte = &pdpt_table[PDPT_INDEX(virt)];
    if (!(*pdpte & PAGE_PRESENT)) return 0;
    
    pt_entry_t *pd_table = (pt_entry_t*)(*pdpte & 0x000FFFFFFFFFF000ULL);
    pt_entry_t *pde = &pd_table[PD_INDEX(virt)];
    if (!(*pde & PAGE_PRESENT)) return 0;
    
    if (*pde & PAGE_HUGE) {
        return (*pde & 0x000FFFFFFFFFF000ULL) + (virt & 0x1FFFFF);
    } else {
        pt_entry_t *pt_table = (pt_entry_t*)(*pde & 0x000FFFFFFFFFF000ULL);
        pt_entry_t *pte = &pt_table[PT_INDEX(virt)];
        if (!(*pte & PAGE_PRESENT)) return 0;
        return (*pte & 0x000FFFFFFFFFF000ULL) + (virt & 0xFFF);
    }
}

// Синхронизация kernel space записей между PML4 (для процессов)
void sync_kernel_mappings(uint64_t dest_pml4_phys, uint64_t src_pml4_phys) {
    pt_entry_t *dest_pml4 = (pt_entry_t*)dest_pml4_phys;
    pt_entry_t *src_pml4 = (pt_entry_t*)src_pml4_phys;
    
    for (int i = 256; i < 512; i++) {  // Только kernel space (верхняя половина)
        if (src_pml4[i] & PAGE_PRESENT) {
            uint64_t table_phys = src_pml4[i] & 0x000FFFFFFFFFF000ULL;
            // Копируем всю таблицу PDPT для этого индекса
            pt_entry_t *src_table = (pt_entry_t*)table_phys;
            
            if (!(dest_pml4[i] & PAGE_PRESENT)) {
                // Выделяем новую PDPT для destination
                uint64_t new_table_phys = pmm_alloc_page();
                if (new_table_phys) {
                    dest_pml4[i] = (new_table_phys & ~0xFFF) | (src_pml4[i] & 0xFFF);
                    pt_entry_t *dest_table = (pt_entry_t*)new_table_phys;
                    // Копируем все записи
                    for (int j = 0; j < 512; j++) {
                        dest_table[j] = src_table[j];
                    }
                }
            } else {
                // Обновляем существующую таблицу
                pt_entry_t *dest_table = (pt_entry_t*)(dest_pml4[i] & 0x000FFFFFFFFFF000ULL);
                for (int j = 0; j < 512; j++) {
                    dest_table[j] = src_table[j];
                }
            }
        }
    }
}