#include "elf.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "system/mm/heap.h"
#include "system/process/process.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"

#ifndef PAGE_PS
#define PAGE_PS     0x80    // Page size (2MB / 1GB)
#endif

static void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dest;
}

int elf_validate(const elf64_header_t *header) {
    if (header->magic != ELF_MAGIC) {
        printf("[ELF] Invalid magic: 0x%x\n", header->magic);
        return -1;
    }
    
    if (header->elf_class != ELFCLASS64) {
        printf("[ELF] Not a 64-bit executable\n");
        return -1;
    }
    
    if (header->machine != EM_X86_64) {
        printf("[ELF] Not an x86-64 executable\n");
        return -1;
    }
    
    if (header->type != ET_EXEC && header->type != ET_DYN) {
        printf("[ELF] Not an executable (type=%u)\n", header->type);
        return -1;
    }
    
    if (header->phnum == 0) {
        printf("[ELF] No program headers\n");
        return -1;
    }
    
    return 0;
}

/*
 * Выделение страницы и маппинг в УКАЗАННОМ адресном пространстве (по pml4_phys).
 * НЕ переключает CR3 - работает через identity mapping для доступа к таблицам.
 * 
 * ВАЖНО: Эта функция модифицирует таблицы страниц по указанному pml4_phys.
 * Вызывающий код должен обеспечить, что:
 * 1. Текущий CR3 позволяет через identity mapping читать/писать все 
 *    физические адреса (как выделяемые pmm_alloc_page, так и сам pml4_phys).
 * 2. pml4_phys валиден и указывает на PML4 целевого адресного пространства.
 */
// ============================================================
// ИСПРАВЛЕННАЯ map_page_in_space()
// ============================================================

static int map_page_in_space(uint64_t pml4_phys,
                             uint64_t virt,
                             uint64_t phys,
                             uint64_t flags)
{
    uint64_t *pml4 = (uint64_t*)pml4_phys;

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    // ========================================================
    // PML4 -> PDPT
    // ========================================================

    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {

        uint64_t new_pdpt_phys = pmm_alloc_page();
        if (!new_pdpt_phys)
            return -1;

        uint64_t *new_pdpt = (uint64_t*)new_pdpt_phys;

        for (int i = 0; i < 512; i++)
            new_pdpt[i] = 0;

        pml4[pml4_idx] =
            (new_pdpt_phys & ~0xFFFULL) |
            PAGE_PRESENT |
            PAGE_WRITE |
            PAGE_USER;
    }

    uint64_t *pdpt =
        (uint64_t*)(pml4[pml4_idx] & ~0xFFFULL);

    // ========================================================
    // PDPT -> PD
    // ========================================================

    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {

        uint64_t new_pd_phys = pmm_alloc_page();
        if (!new_pd_phys)
            return -1;

        uint64_t *new_pd = (uint64_t*)new_pd_phys;

        for (int i = 0; i < 512; i++)
            new_pd[i] = 0;

        pdpt[pdpt_idx] =
            (new_pd_phys & ~0xFFFULL) |
            PAGE_PRESENT |
            PAGE_WRITE |
            PAGE_USER;
    }

    uint64_t *pd =
        (uint64_t*)(pdpt[pdpt_idx] & ~0xFFFULL);

    // ========================================================
    // SPLIT 2MB HUGE PAGE
    // ========================================================

    if (pd[pd_idx] & PAGE_PS) {

        uint64_t huge_entry = pd[pd_idx];

        uint64_t phys_2m =
            huge_entry & 0x000FFFFFFFE00000ULL;

        uint64_t orig_flags =
            (huge_entry & 0xFFFULL) & ~PAGE_PS;

        uint64_t new_pt_phys = pmm_alloc_page();
        if (!new_pt_phys)
            return -1;

        uint64_t *new_pt = (uint64_t*)new_pt_phys;

        for (int j = 0; j < 512; j++) {

            new_pt[j] =
                (phys_2m + j * PAGE_SIZE) |
                orig_flags |
                PAGE_PRESENT;
        }

        pd[pd_idx] =
            (new_pt_phys & ~0xFFFULL) |
            PAGE_PRESENT |
            PAGE_WRITE |
            PAGE_USER;
    }

    // ========================================================
    // PT
    // ========================================================

    uint64_t *pt =
        (uint64_t*)(pd[pd_idx] & ~0xFFFULL);

    pt[pt_idx] =
        (phys & ~0xFFFULL) |
        (flags & 0xFFFULL) |
        PAGE_PRESENT;

    return 0;
}

// ============================================================
// НОВАЯ set_page_flags_in_space()
// ============================================================

static int set_page_flags_in_space(uint64_t pml4_phys,
                                   uint64_t virt,
                                   uint64_t flags)
{
    uint64_t *pml4 = (uint64_t*)pml4_phys;

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & PAGE_PRESENT))
        return -1;

    uint64_t *pdpt =
        (uint64_t*)(pml4[pml4_idx] & ~0xFFFULL);

    if (!(pdpt[pdpt_idx] & PAGE_PRESENT))
        return -1;

    uint64_t *pd =
        (uint64_t*)(pdpt[pdpt_idx] & ~0xFFFULL);

    if (!(pd[pd_idx] & PAGE_PRESENT))
        return -1;

    if (pd[pd_idx] & PAGE_PS)
        return -1;

    uint64_t *pt =
        (uint64_t*)(pd[pd_idx] & ~0xFFFULL);

    if (!(pt[pt_idx] & PAGE_PRESENT))
        return -1;

    uint64_t phys = pt[pt_idx] & ~0xFFFULL;

    pt[pt_idx] =
        phys |
        (flags & 0xFFFULL) |
        PAGE_PRESENT;

    return 0;
}

// Загрузка ELF в адресное пространство процесса
void* elf_load_to_process(const void *elf_data,
                          uint64_t elf_size,
                          process_t *proc,
                          const char *name)
{
    if (!elf_data || !elf_size || !proc) {
        printf("[ELF] Invalid parameters\n");
        return NULL;
    }

    const elf64_header_t *header =
        (const elf64_header_t*)elf_data;

    if (elf_validate(header) != 0)
        return NULL;

    printf("[ELF] Loading '%s' into process %u\n",
           name,
           proc->pid);

    uint64_t proc_pml4 = proc->page_table;

    const elf64_program_header_t *ph =
        (const elf64_program_header_t*)
        ((const uint8_t*)elf_data + header->phoff);

    typedef struct {
        uint64_t vaddr;
        uint64_t memsz;
        uint64_t filesz;
        uint64_t offset;
        uint32_t flags;
    } segment_info_t;

    segment_info_t segments[32];
    int seg_count = 0;

    // ========================================================
    // СОБИРАЕМ PT_LOAD
    // ========================================================

    for (int i = 0; i < header->phnum; i++) {

        if (ph[i].type != PT_LOAD)
            continue;

        segments[seg_count].vaddr  = ph[i].vaddr;
        segments[seg_count].memsz  = ph[i].memsz;
        segments[seg_count].filesz = ph[i].filesz;
        segments[seg_count].offset = ph[i].offset;
        segments[seg_count].flags  = ph[i].flags;

        seg_count++;
    }

    // ========================================================
    // МАППИНГ
    // ВАЖНО:
    // ВСЕ СТРАНИЦЫ ВРЕМЕННО WRITABLE
    // ========================================================

    for (int i = 0; i < seg_count; i++) {

        uint64_t seg_start =
            segments[i].vaddr & ~(PAGE_SIZE - 1);

        uint64_t seg_end =
            (segments[i].vaddr +
             segments[i].memsz +
             PAGE_SIZE - 1)
            & ~(PAGE_SIZE - 1);

        uint64_t seg_pages =
            (seg_end - seg_start) / PAGE_SIZE;

        printf("[ELF] Segment %d: 0x%lx - 0x%lx (%lu pages)\n",
               i,
               seg_start,
               seg_end,
               seg_pages);

        for (uint64_t p = 0; p < seg_pages; p++) {

            uint64_t virt =
                seg_start + p * PAGE_SIZE;

            uint64_t phys =
                pmm_alloc_page();

            if (!phys) {
                printf("[ELF] Out of memory\n");
                return NULL;
            }

            // ============================================
            // ВРЕМЕННО WRITE ДЛЯ ВСЕХ
            // ============================================

            uint64_t flags =
                PAGE_USER |
                PAGE_WRITE;

            if (!(segments[i].flags & PF_X))
                flags |= PAGE_NX;

            if (map_page_in_space(proc_pml4,
                                  virt,
                                  phys,
                                  flags) != 0)
            {
                printf("[ELF] map failed\n");
                pmm_free_page(phys);
                return NULL;
            }
        }
    }

    // ========================================================
    // ПЕРЕКЛЮЧАЕМСЯ В ПРОЦЕСС
    // ========================================================

    uint64_t old_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(old_cr3));

    asm volatile("mov %0, %%cr3"
                 :
                 : "r"(proc_pml4)
                 : "memory");

    // ========================================================
    // ZERO + COPY
    // ========================================================

    for (int i = 0; i < seg_count; i++) {

        uint64_t seg_start =
            segments[i].vaddr & ~(PAGE_SIZE - 1);

        uint64_t seg_end =
            (segments[i].vaddr +
             segments[i].memsz +
             PAGE_SIZE - 1)
            & ~(PAGE_SIZE - 1);

        // ================================================
        // ZERO
        // ================================================

        memset((void*)seg_start, 0, seg_end - seg_start);

        // ================================================
        // COPY
        // ================================================

        if (segments[i].filesz > 0) {

            memcpy(
                (void*)segments[i].vaddr,
                (const uint8_t*)elf_data +
                    segments[i].offset,
                segments[i].filesz
            );
        }
    }

    // ========================================================
    // ВОССТАНАВЛИВАЕМ CR3
    // ========================================================

    asm volatile("mov %0, %%cr3"
                 :
                 : "r"(old_cr3)
                 : "memory");

    // ========================================================
    // ВОЗВРАЩАЕМ ФИНАЛЬНЫЕ ПРАВА
    // ========================================================

    for (int i = 0; i < seg_count; i++) {

        uint64_t seg_start =
            segments[i].vaddr & ~(PAGE_SIZE - 1);

        uint64_t seg_end =
            (segments[i].vaddr +
             segments[i].memsz +
             PAGE_SIZE - 1)
            & ~(PAGE_SIZE - 1);

        uint64_t seg_pages =
            (seg_end - seg_start) / PAGE_SIZE;

        uint64_t final_flags =
            PAGE_USER;

        if (segments[i].flags & PF_W)
            final_flags |= PAGE_WRITE;

        if (!(segments[i].flags & PF_X))
            final_flags |= PAGE_NX;

        for (uint64_t p = 0; p < seg_pages; p++) {

            uint64_t virt =
                seg_start + p * PAGE_SIZE;

            set_page_flags_in_space(
                proc_pml4,
                virt,
                final_flags
            );
        }
    }

    asm volatile("mov %0, %%cr3"
                 :
                 : "r"(old_cr3)
                 : "memory");

    printf("[ELF] Loaded successfully\n");

    return (void*)header->entry;
}

// Загрузка ELF и создание процесса
int elf_exec(const void *elf_data, uint64_t elf_size, const char *name) {
    printf("[ELF] DEBUG: Step 0 - entry\n");
    
    asm volatile("cli");
    
    // Сохраняем CR3
    uint64_t cr3_before;
    asm volatile("mov %%cr3, %0" : "=r"(cr3_before));
    printf("[ELF] DEBUG: CR3 before = 0x%lx, kernel_cr3 = 0x%lx, match=%d\n",
           cr3_before, kernel_cr3, (cr3_before == kernel_cr3));
    
    // Создаём процесс
    process_t *proc = process_create(name, NULL);
    if (!proc) {
        printf("[ELF] Failed to create process\n");
        asm volatile("sti");
        return -1;
    }
    
    // Проверяем, что мы всё ещё в kernel_cr3
    uint64_t cr3_after_create;
    asm volatile("mov %%cr3, %0" : "=r"(cr3_after_create));
    printf("[ELF] DEBUG: CR3 after create = 0x%lx, match=%d\n",
           cr3_after_create, (cr3_after_create == kernel_cr3));
    
    printf("[ELF] DEBUG: Trying proc->pid...\n");
    uint32_t pid = proc->pid;
    printf("[ELF] DEBUG: proc->pid = %u (OK!)\n", pid);
    
    printf("[ELF] DEBUG: Trying proc->page_table...\n");
    uint64_t pt = proc->page_table;
    printf("[ELF] DEBUG: proc->page_table = 0x%lx (OK!)\n", pt);
    
    // Загружаем ELF в процесс
    printf("[ELF] DEBUG: Now loading ELF...\n");
    void *entry = elf_load_to_process(elf_data, elf_size, proc, name);
    
    // Убеждаемся, что мы в kernel_cr3
    uint64_t cr3_after_load;
    asm volatile("mov %%cr3, %0" : "=r"(cr3_after_load));
    printf("[ELF] DEBUG: CR3 after load = 0x%lx, match=%d\n",
           cr3_after_load, (cr3_after_load == kernel_cr3));
    
    if (cr3_after_load != kernel_cr3) {
        asm volatile("mov %0, %%cr3" : : "r"(kernel_cr3) : "memory");
    }
    
    if (!entry) {
        printf("[ELF] Failed to load ELF\n");
        proc->state = PROCESS_TERMINATED;
        asm volatile("sti");
        return -1;
    }
    
    // Устанавливаем точку входа
    printf("[ELF] DEBUG: Setting up entry point...\n");
    proc->context.rip = (uint64_t)entry;
    printf("[ELF] DEBUG: proc->context.rip = 0x%lx (OK!)\n", proc->context.rip);
    
    printf("[ELF] DEBUG: ALL OK!\n");
    
    // Запускаем процесс, если это первый пользовательский процесс
    asm volatile("cli");
    switch_to_process(proc);
    
    return 0;
}