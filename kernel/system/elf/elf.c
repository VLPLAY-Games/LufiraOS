#include "elf.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "system/mm/heap.h"
#include "system/process/process.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"

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

// Выделение страницы и маппинг с созданием всех необходимых таблиц
// ПРЕДПОЛАГАЕТСЯ: identity mapping для физических адресов (физический = виртуальный)
static int map_page_with_tables(uint64_t virt, uint64_t phys, uint64_t flags) {
    // Получаем физический адрес PML4 из CR3
    uint64_t pml4_phys;
    asm volatile("mov %%cr3, %0" : "=r"(pml4_phys));
    
    // БЛАГОДАРЯ IDENTITY MAPPING: физический адрес можно использовать как виртуальный
    // Так как paging_init настроил identity mapping для всей физической памяти
    uint64_t *pml4 = (uint64_t*)pml4_phys;
    
    // Индексы в таблицах страниц
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;
    
    // Проверяем/создаём PDPT
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        uint64_t new_pdpt_phys = pmm_alloc_page();
        if (!new_pdpt_phys) return -1;
        
        // Обнуляем новую PDPT (через identity mapping)
        uint64_t *new_pdpt = (uint64_t*)new_pdpt_phys;
        for (int i = 0; i < 512; i++) new_pdpt[i] = 0;
        
        pml4[pml4_idx] = (new_pdpt_phys & ~0xFFF) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
        
        // Инвалидируем TLB для этой записи
        asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }
    
    uint64_t pdpt_phys = pml4[pml4_idx] & ~0xFFF;
    uint64_t *pdpt = (uint64_t*)pdpt_phys;
    
    // Проверяем/создаём Page Directory
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        uint64_t new_pd_phys = pmm_alloc_page();
        if (!new_pd_phys) return -1;
        
        uint64_t *new_pd = (uint64_t*)new_pd_phys;
        for (int i = 0; i < 512; i++) new_pd[i] = 0;
        
        pdpt[pdpt_idx] = (new_pd_phys & ~0xFFF) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
        
        asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }
    
    uint64_t pd_phys = pdpt[pdpt_idx] & ~0xFFF;
    uint64_t *pd = (uint64_t*)pd_phys;
    
    // Проверяем/создаём Page Table
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        uint64_t new_pt_phys = pmm_alloc_page();
        if (!new_pt_phys) return -1;
        
        uint64_t *new_pt = (uint64_t*)new_pt_phys;
        for (int i = 0; i < 512; i++) new_pt[i] = 0;
        
        pd[pd_idx] = (new_pt_phys & ~0xFFF) | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
        
        asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
    }
    
    uint64_t pt_phys = pd[pd_idx] & ~0xFFF;
    uint64_t *pt = (uint64_t*)pt_phys;
    
    // Устанавливаем запись в Page Table
    // Флаги уже включают PAGE_PRESENT, просто ORим для страницы
    uint64_t entry = (phys & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;
    pt[pt_idx] = entry;
    
    // Инвалидируем TLB для этого адреса
    asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
    
    return 0;
}

// Загрузка ELF в адресное пространство процесса
void* elf_load_to_process(const void *elf_data, uint64_t elf_size, 
                          process_t *proc, const char *name) {
    if (!elf_data || !elf_size || !proc) {
        printf("[ELF] Invalid parameters\n");
        return NULL;
    }
    
    const elf64_header_t *header = (const elf64_header_t *)elf_data;
    
    if (elf_validate(header) != 0) {
        return NULL;
    }
    
    printf("[ELF] Loading '%s' into process %u: %u KB, entry=0x%lx\n", 
           name, proc->pid, (uint32_t)(elf_size / 1024), header->entry);
    
    // Сохраняем старый CR3
    uint64_t old_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(old_cr3));
    
    // Переключаемся на PML4 процесса
    asm volatile("mov %0, %%cr3" : : "r"(proc->page_table) : "memory");
    
    uint64_t entry_point = header->entry;
    uint64_t max_addr = 0;
    
    // Загружаем программные сегменты
    const elf64_program_header_t *ph = 
        (const elf64_program_header_t *)((const uint8_t *)elf_data + header->phoff);
    
    for (int i = 0; i < header->phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        
        uint64_t segment_addr = ph[i].vaddr;
        uint64_t segment_memsz = ph[i].memsz;
        uint64_t segment_filesz = ph[i].filesz;
        uint64_t segment_offset = ph[i].offset;
        uint32_t segment_flags = ph[i].flags;
        
        // Вычисляем выровненные границы страниц
        uint64_t seg_start = segment_addr & ~(PAGE_SIZE - 1);
        uint64_t seg_end = (segment_addr + segment_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint64_t seg_pages = (seg_end - seg_start) / PAGE_SIZE;
        
        printf("[ELF]   Segment %d: vaddr=0x%lx..0x%lx (%lu pages) flags=%c%c%c\n",
               i, seg_start, seg_end, seg_pages,
               (segment_flags & PF_R) ? 'R' : '-',
               (segment_flags & PF_W) ? 'W' : '-',
               (segment_flags & PF_X) ? 'X' : '-');
        
        // Выделяем и отображаем страницы для сегмента
        for (uint64_t page_idx = 0; page_idx < seg_pages; page_idx++) {
            uint64_t virt = seg_start + page_idx * PAGE_SIZE;
            uint64_t phys = pmm_alloc_page();
            
            if (!phys) {
                printf("[ELF] Failed to allocate physical page for 0x%lx\n", virt);
                asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
                return NULL;
            }
            
            // Определяем флаги страницы
            uint64_t page_flags = PAGE_WRITE | PAGE_USER;
            if (!(segment_flags & PF_X)) {
                page_flags |= PAGE_NX;
            }
            
            // Маппим страницу с созданием таблиц
            if (map_page_with_tables(virt, phys, page_flags) != 0) {
                printf("[ELF] Failed to map page 0x%lx\n", virt);
                pmm_free_page(phys);
                asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
                return NULL;
            }
            
            // Обнуляем страницу (теперь она доступна для записи через identity mapping)
            uint64_t *page_ptr = (uint64_t*)virt;
            for (int j = 0; j < PAGE_SIZE / 8; j++) {
                page_ptr[j] = 0;
            }
        }
        
        // Копируем данные из файла (только в пределах filesz)
        if (segment_filesz > 0) {
            for (uint64_t offset = 0; offset < segment_filesz; offset++) {
                uint64_t addr = segment_addr + offset;
                uint64_t file_offset = segment_offset + offset;
                if (file_offset < elf_size) {
                    ((uint8_t*)addr)[0] = ((const uint8_t*)elf_data)[file_offset];
                } else {
                    printf("[ELF] WARNING: File read out of bounds at offset 0x%lx\n", file_offset);
                    break;
                }
            }
        }
        
        if (segment_addr + segment_memsz > max_addr) {
            max_addr = segment_addr + segment_memsz;
        }
    }
    
    // Возвращаем старый CR3
    asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
    
    printf("[ELF] Loaded successfully, entry point: 0x%lx, max addr: 0x%lx\n", 
           entry_point, max_addr);
    
    return (void*)entry_point;
}

// Загрузка ELF и создание процесса
int elf_exec(const void *elf_data, uint64_t elf_size, const char *name) {
    if (!elf_data || !elf_size || !name) {
        printf("[ELF] Invalid parameters\n");
        return -1;
    }
    
    const elf64_header_t *header = (const elf64_header_t *)elf_data;
    
    if (elf_validate(header) != 0) {
        printf("[ELF] Invalid ELF header for %s\n", name);
        return -1;
    }
    
    printf("[ELF] Executing '%s' from memory (%u bytes)\n", name, (uint32_t)elf_size);
    
    // Отключаем прерывания
    asm volatile("cli");
    
    // ШАГ 1: Создаём процесс с его собственным PML4
    process_t *proc = process_create(name, NULL);
    
    if (!proc) {
        printf("[ELF] Failed to create process for %s\n", name);
        asm volatile("sti");
        return -1;
    }
    
    printf("[ELF] Created process %u with PML4 0x%lx\n", proc->pid, proc->page_table);
    
    // ШАГ 2: Загружаем ELF в адресное пространство процесса
    void *entry = elf_load_to_process(elf_data, elf_size, proc, name);
    
    if (!entry) {
        printf("[ELF] Failed to load ELF into process %u\n", proc->pid);
        proc->state = PROCESS_TERMINATED;
        asm volatile("sti");
        return -1;
    }
    
    // ШАГ 3: Настраиваем точку входа
    proc->context.rip = (uint64_t)entry;
    
    printf("[ELF] Process %u entry point set to 0x%lx\n", proc->pid, (uint64_t)entry);
    printf("[ELF] User stack at 0x%lx\n", proc->stack_base);
    
    // Включаем прерывания
    asm volatile("sti");
    
    // ШАГ 4: Если мы в idle процессе, сразу переключаемся
    if (current_process == NULL || current_process->pid == 0) {
        printf("[ELF] Switching to new process %u\n", proc->pid);
        switch_to_process(proc);
    } else {
        printf("[ELF] Process %u ready, waiting for scheduler\n", proc->pid);
    }
    
    return 0;
}