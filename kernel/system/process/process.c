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

// Выделение Ring 0 стека (исправленная версия)
static uint64_t allocate_ring0_stack(process_t *proc) {
    size_t num_pages = 4;  // 16 KB
    uint64_t *pages = (uint64_t*)kmalloc(sizeof(uint64_t) * num_pages);
    if (!pages) return 0;
    
    // Выделяем физические страницы
    for (size_t i = 0; i < num_pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            // Освобождаем уже выделенные страницы
            for (size_t j = 0; j < i; j++) {
                pmm_free_page(pages[j]);
            }
            kfree(pages);
            return 0;
        }
        pages[i] = phys;
    }
    
    // Сохраняем массив страниц в структуре процесса
    proc->ring0_stack_pages = (uint64_t)pages;
    
    // Возвращаем верхушку стека (последняя страница + PAGE_SIZE)
    return pages[num_pages - 1] + PAGE_SIZE;
}

// Освобождение Ring 0 стека (исправленная версия)
static void free_ring0_stack(process_t *proc) {
    if (!proc->ring0_stack_pages) return;
    
    uint64_t *pages = (uint64_t*)proc->ring0_stack_pages;
    size_t num_pages = 4;
    
    for (size_t i = 0; i < num_pages; i++) {
        if (pages[i]) {
            pmm_free_page(pages[i]);
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
        asm volatile("cli");
    }
}

static uint64_t create_address_space(uint64_t kernel_pml4_phys) {
    uint64_t new_pml4_phys = pmm_alloc_page();
    if (!new_pml4_phys) return 0;
    
    uint64_t *new_pml4 = (uint64_t*)new_pml4_phys;
    uint64_t *kernel_pml4 = (uint64_t*)kernel_pml4_phys;
    
    // Копируем все записи из ядерного PML4
    for (int i = 0; i < 512; i++) {
        new_pml4[i] = kernel_pml4[i];
    }
    
    return new_pml4_phys;
}

void process_init(void) {
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
    
    asm volatile("mov %%cr3, %0" : "=r"(idle_process->page_table));
    
    process_list = idle_process;
    current_process = idle_process;
    
    printf("[PROCESS] Process manager initialized\n");
}

static uint64_t allocate_user_stack(size_t size, uint64_t pml4_phys) {
    size_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    uint64_t old_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(old_cr3));
    asm volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
    
    // Выделяем стек в userspace-адресе (0x0000700000000000)
    uint64_t stack_virt = 0x0000700000000000ULL;
    
    for (size_t i = 0; i < num_pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            // Освобождаем выделенные
            for (size_t j = 0; j < i; j++) {
                uint64_t v = stack_virt + j * PAGE_SIZE;
                uint64_t p = get_physical_address(v);
                if (p) {
                    unmap_page(v);
                    pmm_free_page(p);
                }
            }
            asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
            return 0;
        }
        
        uint64_t virt = stack_virt + i * PAGE_SIZE;
        if (map_page(virt, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
            pmm_free_page(phys);
            asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
            return 0;
        }
    }
    
    asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
    
    return stack_virt + num_pages * PAGE_SIZE;
}

process_t* process_create(const char *name, void (*entry)(void)) {
    process_t *proc = (process_t*)kmalloc(sizeof(process_t));
    if (!proc) return NULL;
    
    proc->pid = next_pid++;
    proc->state = PROCESS_READY;
    
    for (int i = 0; i < 31 && name[i]; i++) proc->name[i] = name[i];
    proc->name[31] = '\0';
    
    uint64_t kernel_pml4;
    asm volatile("mov %%cr3, %0" : "=r"(kernel_pml4));
    
    uint64_t new_pml4 = create_address_space(kernel_pml4);
    if (!new_pml4) {
        kfree(proc);
        return NULL;
    }
    proc->page_table = new_pml4;
    
    // Выделяем Ring 0 стек
    proc->ring0_stack = allocate_ring0_stack(proc);
    if (!proc->ring0_stack) {
        pmm_free_page(new_pml4);
        kfree(proc);
        return NULL;
    }
    
    // Выделяем пользовательский стек
    proc->stack_size = 16384;
    proc->stack_base = allocate_user_stack(proc->stack_size, new_pml4);
    if (!proc->stack_base) {
        pmm_free_page(new_pml4);
        free_ring0_stack(proc);
        kfree(proc);
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
    *stack_ptr = 0x202;  // RFLAGS с IF=1
    
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
    
    return proc;
}

void process_exit(void) {
    // Сохраняем указатель на текущий процесс ДО изменения состояния
    process_t *exiting_process = current_process;
    
    printf("\n[PROCESS] Process %u ('%s') exiting\n", 
           exiting_process->pid, exiting_process->name);
    
    // Запрещаем прерывания
    asm volatile("cli");
    
    // Помечаем как завершённый
    exiting_process->state = PROCESS_TERMINATED;
    
    // ОЧИЩАЕМ current_process, чтобы планировщик не использовал freed память
    current_process = NULL;
    
    // Освобождаем пользовательский стек
    if (exiting_process->stack_base > 0 && exiting_process->stack_size > 0) {
        uint64_t stack_start = exiting_process->stack_base - exiting_process->stack_size;
        size_t num_pages = exiting_process->stack_size / PAGE_SIZE;
        
        // Временно переключаемся на адресное пространство процесса
        uint64_t old_cr3;
        asm volatile("mov %%cr3, %0" : "=r"(old_cr3));
        asm volatile("mov %0, %%cr3" : : "r"(exiting_process->page_table) : "memory");
        
        for (size_t i = 0; i < num_pages; i++) {
            uint64_t virt = stack_start + i * PAGE_SIZE;
            uint64_t phys = get_physical_address(virt);
            if (phys) {
                unmap_page(virt);
                pmm_free_page(phys);
            }
        }
        
        asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
    }
    
    // Освобождаем Ring 0 стек
    if (exiting_process->ring0_stack_pages) {
        free_ring0_stack(exiting_process);
    }
    
    // Освобождаем PML4 (только если это не ядерный)
    if (exiting_process->page_table != kernel_cr3 && exiting_process->page_table != 0) {
        printf("[PROCESS] Freeing PML4: 0x%lx\n", exiting_process->page_table);
        pmm_free_page(exiting_process->page_table);
        exiting_process->page_table = 0;
    }
    
    // Разрешаем прерывания
    asm volatile("sti");
    
    // Передаём управление планировщику
    schedule();
    
    // Сюда никогда не должны попасть
    while (1) {
        asm volatile("hlt");
    }
}

// Очистка завершённых процессов (зомби-процессов)
void process_reap(void) {
    // Запрещаем прерывания
    irq_disable();
    
    if (!process_list) {
        irq_enable();
        return;
    }
    
    // Защита от повреждённого списка
    const uint32_t MAX_ITERATIONS = 10000;
    uint32_t iterations = 0;
    
    process_t *p = process_list;
    process_t *start = p;
    process_t *prev = NULL;
    
    // БЕЗОПАСНОЕ удаление с обходом повреждённых указателей
    while (p && iterations++ < MAX_ITERATIONS) {
        // Сохраняем следующий до удаления
        process_t *next_process = p->next;
        
        // Проверка на зацикливание
        if (next_process == p && p != idle_process) {
            printf("[PROCESS] WARNING: Process points to itself\n");
            break;
        }
        
        // Удаляем только зомби (не idle)
        if (p->state == PROCESS_TERMINATED && p != idle_process) {
            printf("[PROCESS] Reaping zombie PID %u ('%s')\n", p->pid, p->name);
            
            // Удаляем из списка
            if (prev == NULL) {
                // Удаляем голову списка
                if (p->next == p) {
                    // Единственный процесс
                    process_list = idle_process;
                    if (idle_process) {
                        idle_process->next = idle_process;
                    } else {
                        process_list = NULL;
                    }
                } else {
                    // Находим предыдущий
                    process_t *last = process_list;
                    uint32_t find_iter = 0;
                    while (last->next != process_list && find_iter++ < MAX_ITERATIONS) {
                        last = last->next;
                    }
                    if (last) {
                        process_list = p->next;
                        last->next = process_list;
                    }
                }
                
                // Освобождаем память процесса
                kfree(p);
                p = process_list;
                prev = NULL;
                continue;
            } else {
                // Удаляем из середины/конца
                prev->next = p->next;
                kfree(p);
                p = prev->next;
                continue;
            }
        }
        
        // Переход к следующему
        prev = p;
        p = next_process;
        
        // Защита от зацикливания
        if (p == start) {
            break;
        }
        if (p == NULL) {
            break;
        }
    }
    
    if (iterations >= MAX_ITERATIONS) {
        printf("[PROCESS] WARNING: Process list may be corrupted\n");
    }
    
    // Оставляем прерывания запрещёнными - их включит вызывающая функция
    // irq_enable() будет вызван в schedule() ПОСЛЕ switch_to_process()
}

void schedule(void) {
    // Проверяем, разрешены ли прерывания
    uint64_t rflags;
    asm volatile("pushfq; pop %0" : "=r"(rflags));
    if (!(rflags & 0x200)) {
        return;  // Прерывания запрещены, не переключаемся
    }
    
    // Запрещаем прерывания для атомарной работы
    irq_disable();
    
    // Очищаем зомби-процессы (прерывания остаются запрещёнными)
    process_reap();
    
    // Если current_process был обнулён в process_exit, восстанавливаем его через список
    if (current_process == NULL && process_list != NULL) {
        current_process = process_list;
    }
    
    if (current_process == NULL) {
        irq_enable();
        return;
    }
    
    // Поиск следующего готового процесса
    process_t *next = current_process->next;
    int tries = 0;
    const int MAX_TRIES = 100;
    
    while (next && (next->state != PROCESS_READY) && tries < MAX_TRIES) {
        next = next->next;
        tries++;
        if (next == current_process) break;
    }
    
    // Если не нашли готовый процесс, используем idle
    if (!next || next->state != PROCESS_READY || tries >= MAX_TRIES) {
        if (idle_process && idle_process != current_process) {
            next = idle_process;
        } else {
            irq_enable();
            return;
        }
    }
    
    // Если тот же процесс, ничего не делаем
    if (next == current_process) {
        irq_enable();
        return;
    }
    
    // Устанавливаем RSP0 для нового процесса
    if (next->ring0_stack > 0) {
        tss_set_rsp0(next->ring0_stack);
    }
    
    // Переключаем контекст (прерывания остаются запрещёнными)
    switch_to_process(next);
    
    // Разрешаем прерывания ПОСЛЕ возврата (когда этот процесс снова запланирован)
    irq_enable();
}

void switch_to_process(process_t *next) {
    if (!next) return;
    
    process_t *prev = current_process;
    
    // Обновляем состояния
    if (prev && prev->state == PROCESS_RUNNING) {
        prev->state = PROCESS_READY;
    }
    next->state = PROCESS_RUNNING;
    
    // Сохраняем текущий CR3 перед переключением
    uint64_t old_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(old_cr3));
    
    // Если меняется адресное пространство, обновляем CR3
    if (prev && prev->page_table != next->page_table) {
        asm volatile("mov %0, %%cr3" : : "r"(next->page_table) : "memory");
    }
    
    current_process = next;
    
    // Выполняем переключение контекста
    context_switch(prev ? &prev->context : NULL, &next->context);
    
    // Восстанавливаем CR3 после возврата (если нужно)
    uint64_t new_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(new_cr3));
    if (new_cr3 != old_cr3 && current_process) {
        // CR3 уже должен быть правильным из-за переключения выше
        // Эта проверка просто для отладки
    }
}