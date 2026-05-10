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

// Функция-обёртка для запуска ELF в отдельном процессе
typedef void (*elf_entry_t)(void);

static void elf_wrapper(void) {
    // Эта функция никогда не вызывается напрямую
    // Адрес точки входа подменяется в стеке при создании процесса
    while (1) __asm__("hlt");
}

void* elf_load(const void *elf_data, uint64_t elf_size, const char *name) {
    if (!elf_data || !elf_size) {
        printf("[ELF] No data\n");
        return NULL;
    }
    
    const elf64_header_t *header = (const elf64_header_t *)elf_data;
    
    // Проверяем заголовок
    if (elf_validate(header) != 0) {
        return NULL;
    }
    
    printf("[ELF] Loading '%s': %u KB, entry=0x%lx\n", 
           name, (uint32_t)(elf_size / 1024), header->entry);
    
    // Получаем текущий PML4
    uint64_t pml4_phys;
    asm volatile("mov %%cr3, %0" : "=r"(pml4_phys));
    
    // Временно переключаемся на адресное пространство процесса
    // (будет создано в process_create)
    // Пока используем текущее для выделения памяти
    
    // Загружаем программные сегменты
    const elf64_program_header_t *ph = 
        (const elf64_program_header_t *)((const uint8_t *)elf_data + header->phoff);
    
    uint64_t load_base = 0;
    uint64_t max_addr = 0;
    
    for (int i = 0; i < header->phnum; i++) {
        if (ph[i].type != PT_LOAD) continue;
        
        // Для PIE используем адреса как есть (обычно 0x0-based)
        // Для ET_EXEC используем vaddr напрямую
        uint64_t segment_addr = ph[i].vaddr;
        
        // Выделяем и отображаем память
        uint64_t seg_start = segment_addr & ~(PAGE_SIZE - 1);  // page align down
        uint64_t seg_end = (segment_addr + ph[i].memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint64_t seg_size = seg_end - seg_start;
        
        uint64_t pages = seg_size / PAGE_SIZE;
        if (seg_size % PAGE_SIZE) pages++;
        
        printf("[ELF]   Segment %d: vaddr=0x%lx size=%u flags=%c%c%c\n",
               i, segment_addr, (uint32_t)ph[i].memsz,
               (ph[i].flags & PF_R) ? 'R' : '-',
               (ph[i].flags & PF_W) ? 'W' : '-',
               (ph[i].flags & PF_X) ? 'X' : '-');
        
        for (uint64_t j = 0; j < pages; j++) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) {
                printf("[ELF] Failed to allocate memory\n");
                return NULL;
            }
            
            uint64_t virt = seg_start + j * PAGE_SIZE;
            
            // Определяем флаги страницы
            uint64_t page_flags = PAGE_PRESENT | PAGE_USER;
            if (ph[i].flags & PF_W) page_flags |= PAGE_WRITE;
            // Для исполняемых страниц не ставим NX (по умолчанию исполнение разрешено)
            
            if (map_page(virt, phys, page_flags) != 0) {
                printf("[ELF] Failed to map page 0x%lx\n", virt);
                pmm_free_page(phys);
                return NULL;
            }
            
            // Копируем данные из файла
            uint64_t file_offset = ph[i].offset + j * PAGE_SIZE;
            uint64_t virt_page_start = virt;
            uint64_t copy_start = segment_addr > virt_page_start ? segment_addr : virt_page_start;
            uint64_t copy_size = 0;
            
            if (segment_addr + ph[i].filesz > virt_page_start) {
                uint64_t end = segment_addr + ph[i].filesz;
                if (end > virt_page_start + PAGE_SIZE) end = virt_page_start + PAGE_SIZE;
                if (end > copy_start) {
                    copy_size = end - copy_start;
                    uint64_t file_offset_bytes = file_offset + (copy_start - virt_page_start);
                    memcpy((void *)copy_start, 
                           (const uint8_t *)elf_data + file_offset_bytes, 
                           copy_size);
                }
            }
            
            // Зануляем оставшуюся часть
            if (copy_size < PAGE_SIZE) {
                uint64_t zero_start = virt_page_start + copy_size;
                uint64_t zero_size = PAGE_SIZE - copy_size;
                if (zero_start < seg_end) {
                    memset((void *)zero_start, 0, zero_size);
                }
            }
            
            if (segment_addr + ph[i].memsz > virt_page_start + PAGE_SIZE) {
                uint64_t bss_start = virt_page_start + PAGE_SIZE;
                if (bss_start < seg_end) {
                    memset((void *)bss_start, 0, seg_end - bss_start);
                }
            }
        }
        
        if (segment_addr > max_addr) max_addr = segment_addr + ph[i].memsz;
    }
    
    // Возвращаем точку входа
    void *entry = (void *)header->entry;
    
    printf("[ELF] Loaded successfully, entry point: 0x%lx\n", (uint64_t)entry);
    
    return entry;
}

// Загрузка ELF и создание процесса
int elf_exec(const void *elf_data, uint64_t elf_size, const char *name) {
    // Загружаем ELF в текущее адресное пространство
    void *entry = elf_load(elf_data, elf_size, name);
    if (!entry) {
        printf("[ELF] Failed to load %s\n", name);
        return -1;
    }
    
    // Создаём процесс с точкой входа из ELF
    process_t *proc = process_create(name, entry);
    if (!proc) {
        printf("[ELF] Failed to create process for %s\n", name);
        return -1;
    }
    
    // Переключаемся на новый процесс
    switch_to_process(proc);
    
    return 0;
}