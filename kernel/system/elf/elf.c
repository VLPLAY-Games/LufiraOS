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
    // Проверяем magic
    if (header->magic != ELF_MAGIC) {
        printf("[ELF] Invalid magic: 0x%x\n", header->magic);
        return -1;
    }
    
    // Проверяем класс (64-bit)
    if (header->elf_class != ELFCLASS64) {
        printf("[ELF] Not a 64-bit executable\n");
        return -1;
    }
    
    // Проверяем архитектуру
    if (header->machine != EM_X86_64) {
        printf("[ELF] Not an x86-64 executable\n");
        return -1;
    }
    
    // Проверяем тип (ET_EXEC или ET_DYN для PIE)
    if (header->type != ET_EXEC && header->type != ET_DYN) {
        printf("[ELF] Not an executable (type=%u)\n", header->type);
        return -1;
    }
    
    // Должен быть хотя бы один заголовок
    if (header->phnum == 0) {
        printf("[ELF] No program headers\n");
        return -1;
    }
    
    return 0;
}

// Загрузка ELF в адресное пространство процесса
// ВНИМАНИЕ: Вызывается когда CR3 уже указывает на page_table процесса!
void* elf_load_to_process(const void *elf_data, uint64_t elf_size, 
                          process_t *proc, const char *name) {
    if (!elf_data || !elf_size || !proc) {
        printf("[ELF] Invalid parameters\n");
        return NULL;
    }
    
    const elf64_header_t *header = (const elf64_header_t *)elf_data;
    
    // Проверяем заголовок
    if (elf_validate(header) != 0) {
        return NULL;
    }
    
    printf("[ELF] Loading '%s' into process %u: %u KB, entry=0x%lx\n", 
           name, proc->pid, (uint32_t)(elf_size / 1024), header->entry);
    
    // Сохраняем старый CR3 (должен быть PML4 процесса!)
    uint64_t old_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(old_cr3));
    
    // Убеждаемся, что мы в правильном адресном пространстве
    if (old_cr3 != proc->page_table) {
        printf("[ELF] WARNING: Current CR3 (0x%lx) != process CR3 (0x%lx)\n",
               old_cr3, proc->page_table);
        asm volatile("mov %0, %%cr3" : : "r"(proc->page_table) : "memory");
    }
    
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
        
        // Вычисляем границы страниц
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
                return NULL;
            }
            
            // Определяем флаги страницы
            uint64_t page_flags = PAGE_PRESENT | PAGE_USER;
            if (segment_flags & PF_W) page_flags |= PAGE_WRITE;
            // NX бит для неисполняемых страниц (если нужно)
            if (!(segment_flags & PF_X)) {
                page_flags |= PAGE_NX;
            }
            
            // Маппим страницу
            if (map_page(virt, phys, page_flags) != 0) {
                printf("[ELF] Failed to map page 0x%lx\n", virt);
                pmm_free_page(phys);
                return NULL;
            }
            
            // Обнуляем страницу
            memset((void*)virt, 0, PAGE_SIZE);
        }
        
        // Копируем данные из файла в загруженные страницы
        uint64_t copy_start = segment_addr;
        uint64_t copy_end = segment_addr + segment_filesz;
        
        // Копируем побайтово с проверкой границ
        for (uint64_t addr = copy_start; addr < copy_end; addr++) {
            uint64_t file_offset = segment_offset + (addr - segment_addr);
            if (file_offset < elf_size) {
                *(uint8_t*)addr = ((const uint8_t*)elf_data)[file_offset];
            } else {
                printf("[ELF] WARNING: File read out of bounds at offset 0x%lx\n", file_offset);
                break;
            }
        }
        
        // BSS часть (memsz > filesz) уже обнулена при выделении страниц
        
        if (segment_addr + segment_memsz > max_addr) {
            max_addr = segment_addr + segment_memsz;
        }
    }
    
    // Восстанавливаем CR3 (на всякий случай)
    if (old_cr3 != proc->page_table) {
        asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
    }
    
    printf("[ELF] Loaded successfully, entry point: 0x%lx, max addr: 0x%lx\n", 
           entry_point, max_addr);
    
    // Возвращаем точку входа
    return (void*)entry_point;
}

// Загрузка ELF и создание процесса (правильная архитектура)
int elf_exec(const void *elf_data, uint64_t elf_size, const char *name) {
    if (!elf_data || !elf_size || !name) {
        printf("[ELF] Invalid parameters\n");
        return -1;
    }
    
    const elf64_header_t *header = (const elf64_header_t *)elf_data;
    
    // Проверяем ELF перед созданием процесса
    if (elf_validate(header) != 0) {
        printf("[ELF] Invalid ELF header for %s\n", name);
        return -1;
    }
    
    printf("[ELF] Executing '%s' from memory (%u bytes)\n", name, (uint32_t)elf_size);
    
    // Временно отключаем прерывания
    asm volatile("cli");
    
    // ============================================================
    // ШАГ 1: Создаём процесс с его собственным PML4
    // ============================================================
    process_t *proc = process_create(name, NULL);
    
    if (!proc) {
        printf("[ELF] Failed to create process for %s\n", name);
        asm volatile("sti");
        return -1;
    }
    
    printf("[ELF] Created process %u with PML4 0x%lx\n", proc->pid, proc->page_table);
    
    // ============================================================
    // ШАГ 2: Сохраняем текущий CR3 и переключаемся на PML4 процесса
    // ============================================================
    uint64_t old_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(old_cr3));
    asm volatile("mov %0, %%cr3" : : "r"(proc->page_table) : "memory");
    
    printf("[ELF] Switched to process CR3 (was 0x%lx)\n", old_cr3);
    
    // ============================================================
    // ШАГ 3: Загружаем ELF в адресное пространство процесса
    // ============================================================
    void *entry = elf_load_to_process(elf_data, elf_size, proc, name);
    
    // ============================================================
    // ШАГ 4: Возвращаем старый CR3
    // ============================================================
    asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
    
    if (!entry) {
        printf("[ELF] Failed to load ELF into process %u\n", proc->pid);
        
        // Помечаем процесс как завершённый
        proc->state = PROCESS_TERMINATED;
        asm volatile("sti");
        return -1;
    }
    
    // ============================================================
    // ШАГ 5: Настраиваем точку входа в контексте процесса
    // ============================================================
    proc->context.rip = (uint64_t)entry;
    
    // Настраиваем стек пользователя
    // Стек уже выделен в process_create (0x700000000000)
    // Настраиваем фрейм, который sysret вернёт в пользовательский режим
    
    printf("[ELF] Process %u entry point set to 0x%lx\n", proc->pid, (uint64_t)entry);
    printf("[ELF] User stack at 0x%lx\n", proc->stack_base);
    
    // ============================================================
    // ШАГ 6: Запускаем планировщик
    // ============================================================
    asm volatile("sti");
    
    // Если мы сейчас в idle процессе, сразу переключаемся
    if (current_process == NULL || current_process->pid == 0) {
        printf("[ELF] Switching to new process %u\n", proc->pid);
        switch_to_process(proc);
    } else {
        // Иначе планировщик сам переключится на таймере
        printf("[ELF] Process %u ready, waiting for scheduler\n", proc->pid);
    }
    
    return 0;
}