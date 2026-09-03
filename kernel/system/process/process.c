#include "process.h"
#include "system/mm/heap.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "system/cpu/gdt.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"

static void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static process_t *process_list = NULL;
process_t *current_process = NULL;
uint64_t current_kernel_rsp = 0; // Глобальная переменная для asm
static uint32_t next_pid = 1;
static process_t *idle_process = NULL;
uint64_t kernel_cr3 = 0;

// Глобальный счётчик для вложенных запретов прерываний
static volatile uint32_t irq_disable_counter = 0;

// Вспомогательные функции для управления прерываниями
static inline void irq_disable(void) {
    asm volatile("cli");
    irq_disable_counter++;
}

static inline void irq_enable(void) {
    if (irq_disable_counter > 0) {
        irq_disable_counter--;
        if (irq_disable_counter == 0) {
            asm volatile("sti");
        }
    }
}

// Выделение Ring 0 стека (не требует переключения CR3, так как ядерная память видна везде)
// process.c - НОВАЯ ВЕРСИЯ
// process.c - исправленная allocate_ring0_stack
static uint64_t allocate_ring0_stack(process_t *proc) {
    uint64_t stack_base = KERNEL_STACK_AREA_START + 
                          (proc->pid * KERNEL_STACK_SIZE);
    size_t num_pages = KERNEL_STACK_SIZE / PAGE_SIZE;
    
    uint64_t *pages = (uint64_t*)kmalloc(sizeof(uint64_t) * num_pages);
    if (!pages) return 0;
    
    // Сохраняем текущий CR3
    uint64_t old_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(old_cr3));
    
    // Переключаемся на PML4 процесса ДО маппинга
    asm volatile("mov %0, %%cr3" : : "r"(proc->page_table) : "memory");
    
    for (size_t i = 0; i < num_pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            // Откат
            for (size_t j = 0; j < i; j++) {
                unmap_page(stack_base + j * PAGE_SIZE);
                pmm_free_page(get_physical_address(stack_base + j * PAGE_SIZE));
            }
            asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
            kfree(pages);
            return 0;
        }
        
        uint64_t virt = stack_base + i * PAGE_SIZE;
        // Теперь маппим в адресное пространство процесса
        if (map_page(virt, phys, PAGE_PRESENT | PAGE_WRITE) != 0) {
            pmm_free_page(phys);
            for (size_t j = 0; j < i; j++) {
                unmap_page(stack_base + j * PAGE_SIZE);
                pmm_free_page(get_physical_address(stack_base + j * PAGE_SIZE));
            }
            asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
            kfree(pages);
            return 0;
        }
        pages[i] = virt;
    }
    
    // Возвращаемся к старому CR3
    asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
    
    proc->ring0_stack_pages = (uint64_t)pages;
    return stack_base + KERNEL_STACK_SIZE;
}

// Освобождение Ring 0 стека
static void free_ring0_stack(process_t *proc) {
    if (!proc->ring0_stack_pages) return;
    
    uint64_t *pages = (uint64_t*)proc->ring0_stack_pages;
    uint64_t stack_base = KERNEL_STACK_AREA_START + 
                          (proc->pid * KERNEL_STACK_SIZE);
    size_t num_pages = KERNEL_STACK_SIZE / PAGE_SIZE;
    
    for (size_t i = 0; i < num_pages; i++) {
        uint64_t virt = stack_base + i * PAGE_SIZE;
        uint64_t phys = get_physical_address(virt);
        if (phys) {
            unmap_page(virt);
            pmm_free_page(phys);
        }
    }
    
    kfree(pages);
    proc->ring0_stack_pages = 0;
    proc->ring0_stack = 0;
}

static void idle_thread(void) {
    while (1) {
        asm volatile("sti");
        asm volatile("hlt");
        schedule();
    }
}

// Создаёт новое адресное пространство на основе КОРНЕВОГО ядерного PML4
static uint64_t create_address_space(uint64_t kernel_pml4_phys) {
    uint64_t new_pml4_phys = pmm_alloc_page();
    if (!new_pml4_phys) return 0;
    
    uint64_t *new_pml4 = (uint64_t*)new_pml4_phys;
    uint64_t *kernel_pml4 = (uint64_t*)kernel_pml4_phys;
    
    // Копируем ВСЕ записи, но с модификацией флагов
    for (int i = 0; i < 512; i++) {
        if (kernel_pml4[i] & PAGE_PRESENT) {
            uint64_t entry = kernel_pml4[i];
            
            // Для user space (0-255) - снимаем флаг USER
            if (i < 256) {
                entry &= ~PAGE_USER;  // Убираем доступ из user mode
            }
            
            new_pml4[i] = entry;
        }
    }
    
    return new_pml4_phys;
}

void process_init(void) {
    // Сохраняем корневой ядерный PML4 на раннем этапе (до загрузки процессов)
    asm volatile("mov %%cr3, %0" : "=r"(kernel_cr3));
    
    idle_process = (process_t*)kmalloc(sizeof(process_t));
    if (!idle_process) return;
    
    idle_process->pid = 0;
    idle_process->state = PROCESS_READY;
    idle_process->stack_base = 0;
    idle_process->stack_size = 0;
    idle_process->next = NULL;
    idle_process->ring0_stack = 0;
    idle_process->ring0_stack_pages = 0;
    
    const char *name = "idle";
    for (int i = 0; i < 31 && name[i]; i++) idle_process->name[i] = name[i];
    idle_process->name[31] = '\0';
    
    // idle использует текущее адресное пространство (ядерное)
    asm volatile("mov %%cr3, %0" : "=r"(idle_process->page_table));
    
    process_list = idle_process;
    idle_process->next = idle_process;
    current_process = idle_process;
    
    printf("[PROCESS] Process manager initialized\n");
}

static uint64_t allocate_user_stack(size_t size, uint64_t pml4_phys, uint32_t pid) {
    // Каждому процессу - свой уникальный виртуальный адрес
    uint64_t stack_virt = USER_STACK_AREA_START + (pid * USER_STACK_SIZE);
    size_t num_pages = size / PAGE_SIZE;
    
    if (num_pages == 0) num_pages = 1;
    
    irq_disable();
    
    uint64_t old_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(old_cr3));
    asm volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
    
    for (size_t i = 0; i < num_pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            // Откат
            for (size_t j = 0; j < i; j++) {
                uint64_t v = stack_virt + j * PAGE_SIZE;
                uint64_t p = get_physical_address(v);
                if (p) {
                    unmap_page(v);
                    pmm_free_page(p);
                }
            }
            asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
            irq_enable();
            return 0;
        }
        
        uint64_t virt = stack_virt + i * PAGE_SIZE;
        if (map_page(virt, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
            pmm_free_page(phys);
            for (size_t j = 0; j < i; j++) {
                uint64_t v = stack_virt + j * PAGE_SIZE;
                uint64_t p = get_physical_address(v);
                if (p) {
                    unmap_page(v);
                    pmm_free_page(p);
                }
            }
            asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
            irq_enable();
            return 0;
        }
    }
    
    asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
    irq_enable();
    
    // Возвращаем ВЕРХНИЙ адрес стека
    return stack_virt + size;
}

process_t* process_create(const char *name, void (*entry)(void)) {
    irq_disable();
    
    process_t *proc = (process_t*)kmalloc(sizeof(process_t));
    if (!proc) {
        irq_enable();
        return NULL;
    }
    
    proc->pid = next_pid++;
    proc->state = PROCESS_READY;
    
    for (int i = 0; i < 31 && name[i]; i++) proc->name[i] = name[i];
    proc->name[31] = '\0';
    
    // Используем КОРНЕВОЙ ядерный PML4 для создания нового адресного пространства
    uint64_t new_pml4 = create_address_space(kernel_cr3);
    if (!new_pml4) {
        kfree(proc);
        irq_enable();
        return NULL;
    }
    proc->page_table = new_pml4;
    
    // Синхронизируем актуальные ядерные маппинги (куча могла расшириться)
    uint64_t current_kernel_pml4;
    asm volatile("mov %%cr3, %0" : "=r"(current_kernel_pml4));
    sync_kernel_mappings(proc->page_table, current_kernel_pml4);
    
    // Выделяем Ring 0 стек
    proc->ring0_stack = allocate_ring0_stack(proc);
    if (!proc->ring0_stack) {
        pmm_free_page(new_pml4);
        kfree(proc);
        irq_enable();
        return NULL;
    }
    
    // Выделяем пользовательский стек
    proc->stack_size = 16384;
    proc->stack_base = allocate_user_stack(proc->stack_size, new_pml4, proc->pid);
    if (!proc->stack_base) {
        pmm_free_page(new_pml4);
        free_ring0_stack(proc);
        kfree(proc);
        irq_enable();
        return NULL;
    }
    
    // Инициализируем контекст
    memset(&proc->context, 0, sizeof(process_context_t));
    
    // Заполняем стек пользователя
    uint64_t old_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(old_cr3));
    asm volatile("mov %0, %%cr3" : : "r"(new_pml4) : "memory");
    
    uint64_t rsp = proc->stack_base;
    rsp -= 8;
    uint64_t *stack_ptr = (uint64_t*)rsp;
    *stack_ptr = (uint64_t)process_exit;
    
    rsp -= 8;
    stack_ptr = (uint64_t*)rsp;
    *stack_ptr = (uint64_t)entry;
    
    rsp -= 8;
    stack_ptr = (uint64_t*)rsp;
    *stack_ptr = 0x202;
    
    asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
    
    proc->context.rsp = rsp;
    proc->context.rip = (uint64_t)entry;
    proc->context.rflags = 0x202;
    proc->context.cr3 = new_pml4;
    
    // Добавляем в список
    proc->next = NULL;
    if (!process_list) {
        process_list = proc;
        proc->next = proc;
    } else {
        process_t *last = process_list;
        while (last->next && last->next != process_list) {
            last = last->next;
        }
        last->next = proc;
        proc->next = process_list;
    }
    
    printf("[PROCESS] Created '%s' (PID %u, Ring 3, user stack: 0x%lx)\n", 
           name, proc->pid, proc->stack_base);
    
    irq_enable();
    return proc;
}

void process_exit(void) {
    process_t *exiting_process = current_process;

    if (!exiting_process) {
        while (1)
            asm volatile("hlt");
    }

    printf("\n[PROCESS] Process %u ('%s') exiting\n",
           exiting_process->pid,
           exiting_process->name);

    /*
     * НИЧЕГО НЕ ОСВОБОЖДАЕМ ЗДЕСЬ.
     *
     * Мы всё ещё выполняемся на ring0 stack этого процесса.
     *
     * Нельзя:
     *   - освобождать ring0 stack;
     *   - освобождать его PML4;
     *   - уничтожать page tables.
     *
     * Только помечаем процесс завершённым.
     */

    irq_disable();

    exiting_process->state = PROCESS_TERMINATED;

    /*
     * Передаём управление scheduler.
     *
     * ВАЖНО:
     * current_process оставляем как exiting_process.
     *
     * schedule() увидит TERMINATED и переключится
     * на следующий READY процесс.
     */
    schedule();

    /*
     * Сюда нормальный процесс больше не должен вернуться.
     */
    while (1) {
        asm volatile("hlt");
    }
}

void process_reap(void) {
    if (!process_list) return;
    
    const uint32_t MAX_ITERATIONS = 10000;
    uint32_t iterations = 0;
    
    process_t *p = process_list;
    process_t *start = p;
    process_t *prev = NULL;
    
    while (p && iterations++ < MAX_ITERATIONS) {
        process_t *next_process = p->next;
        
        if (next_process == p && p != idle_process) {
            printf("[PROCESS] WARNING: Process points to itself\n");
            break;
        }
        
        if (p->state == PROCESS_TERMINATED && p != idle_process) {
            printf("[PROCESS] Reaping zombie PID %u ('%s')\n", p->pid, p->name);
            
            if (prev == NULL) {
                if (p->next == p) {
                    if (idle_process) {
                        process_list = idle_process;
                        idle_process->next = idle_process;
                    } else {
                        process_list = NULL;
                    }
                } else {
                    process_t *last = p;
                    uint32_t find_iter = 0;
                    while (last->next != p && find_iter++ < MAX_ITERATIONS) {
                        last = last->next;
                    }
                    
                    if (last && last->next == p) {
                        process_list = p->next;
                        last->next = process_list;
                    } else {
                        printf("[PROCESS] ERROR: Corrupted list\n");
                        break;
                    }
                }
                
                kfree(p);
                p = process_list;
                prev = NULL;
                continue;
            } else {
                prev->next = p->next;
                kfree(p);
                p = prev->next;
                continue;
            }
        }
        
        prev = p;
        p = next_process;
        
        if (p == start) break;
        if (p == NULL) break;
    }
}

void schedule(void) {
    uint64_t rflags;
    asm volatile("pushfq; pop %0" : "=r"(rflags));

    if (!(rflags & 0x200)) {
        return;
    }

    irq_disable();

    /*
     * Сначала выбираем следующий процесс.
     * Никакого reap до context switch.
     */

    if (current_process == NULL && process_list != NULL) {
        current_process = process_list;
    }

    if (current_process == NULL) {
        irq_enable();
        return;
    }

    process_t *next = current_process->next;

    int tries = 0;
    const int MAX_TRIES = 100;

    while (next &&
           next->state != PROCESS_READY &&
           tries < MAX_TRIES)
    {
        next = next->next;
        tries++;

        if (next == current_process)
            break;
    }

    if (!next ||
        next->state != PROCESS_READY ||
        tries >= MAX_TRIES)
    {
        if (idle_process &&
            idle_process != current_process)
        {
            next = idle_process;
        }
        else
        {
            irq_enable();
            return;
        }
    }

    if (next == current_process) {
        irq_enable();
        return;
    }

    /*
     * Сейчас происходит реальный уход с ring0 stack
     * завершившегося процесса.
     */
    switch_to_process(next);

    /*
     * После switch_to_process execution продолжится
     * уже в контексте следующего процесса,
     * когда его контекст будет активирован.
     *
     * До этого места НЕ пытаемся освобождать prev.
     */

    irq_enable();
}

void switch_to_process(process_t *next) {
    if (!next) return;
    
    process_t *prev = current_process;
    
    if (prev && prev->state == PROCESS_RUNNING) {
        prev->state = PROCESS_READY;
    }
    next->state = PROCESS_RUNNING;
    
    if (next->ring0_stack > 0) {
        tss_set_rsp0(next->ring0_stack);
    } else {
        static uint64_t fallback_stack[1024];
        tss_set_rsp0((uint64_t)fallback_stack + sizeof(fallback_stack));
    }
    
    current_process = next;
    
    process_context_t *prev_context = (prev && prev != next) ? &prev->context : &idle_process->context;

    current_process = next;
    current_kernel_rsp = next->ring0_stack;
    
    context_switch(prev_context, &next->context);
}