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

// Стек Ring 0 для каждого процесса (для syscall'ов и прерываний)
static uint64_t allocate_ring0_stack(process_t *proc) {
    size_t num_pages = 4;  // 16 KB
    uint64_t first_phys = pmm_alloc_page();
    if (!first_phys) return 0;
    
    for (size_t i = 1; i < num_pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys || phys != first_phys + i * PAGE_SIZE) {
            if (phys) pmm_free_page(phys);
            for (size_t j = 0; j < i; j++) {
                pmm_free_page(first_phys + j * PAGE_SIZE);
            }
            return 0;
        }
    }
    
    return first_phys + num_pages * PAGE_SIZE;
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
    
    // Выделяем Ring 0 стек (для обработчиков прерываний и syscall)
    proc->ring0_stack = allocate_ring0_stack(proc);
    if (!proc->ring0_stack) {
        pmm_free_page(new_pml4);
        kfree(proc);
        return NULL;
    }
    
    // Выделяем пользовательский стек (в userspace-адресе)
    proc->stack_size = 16384;
    proc->stack_base = allocate_user_stack(proc->stack_size, new_pml4);
    if (!proc->stack_base) {
        pmm_free_page(new_pml4);
        // Освободить ring0_stack
        uint64_t r0_start = proc->ring0_stack - 16384;
        for (int i = 0; i < 4; i++) {
            pmm_free_page(r0_start + i * PAGE_SIZE);
        }
        kfree(proc);
        return NULL;
    }
    
    // Устанавливаем RSP0 в TSS (указатель на Ring 0 стек)
    tss_set_rsp0(proc->ring0_stack);
    
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
    printf("\n[PROCESS] Process %u ('%s') exiting\n", 
           current_process->pid, current_process->name);
    
    // Запрещаем прерывания на время очистки
    asm volatile("cli");
    
    // Помечаем как завершённый
    current_process->state = PROCESS_TERMINATED;
    
    // Освобождаем пользовательский стек
    if (current_process->stack_base > 0 && current_process->stack_size > 0) {
        uint64_t stack_start = current_process->stack_base - current_process->stack_size;
        size_t num_pages = current_process->stack_size / PAGE_SIZE;
        
        printf("[PROCESS] Freeing user stack: 0x%lx - 0x%lx (%u pages)\n",
               stack_start, current_process->stack_base, (uint32_t)num_pages);
        
        for (size_t i = 0; i < num_pages; i++) {
            pmm_free_page(stack_start + i * PAGE_SIZE);
        }
    }
    
    // Освобождаем Ring 0 стек
    if (current_process->ring0_stack > 0) {
        size_t ring0_pages = 4;  // 16 KB
        uint64_t r0_start = current_process->ring0_stack - ring0_pages * PAGE_SIZE;
        
        printf("[PROCESS] Freeing ring0 stack: 0x%lx - 0x%lx\n",
               r0_start, current_process->ring0_stack);
        
        for (size_t i = 0; i < ring0_pages; i++) {
            pmm_free_page(r0_start + i * PAGE_SIZE);
        }
    }
    
    // Освобождаем PML4 (только если это не ядерный)
    if (current_process->page_table != kernel_cr3 && current_process->page_table != 0) {
        printf("[PROCESS] Freeing PML4: 0x%lx\n", current_process->page_table);
        pmm_free_page(current_process->page_table);
    }
    
    // Освобождаем структуру процесса (если она не idle)
    if (current_process != idle_process) {
        // Не освобождаем сразу, планировщик ещё может на него ссылаться
        // Помечаем как "зомби" — будет удалён позже
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

// Очистка завершённых процессов (зомби-процессов)
void process_reap(void) {
    // Запрещаем прерывания с учётом вложенности
    irq_disable();
    
    if (!process_list) {
        irq_enable();
        return;
    }
    
    // Счётчик итераций для защиты от повреждённого списка
    const uint32_t MAX_ITERATIONS = 10000;
    uint32_t iterations = 0;
    
    process_t *p = process_list;
    process_t *start = p;
    process_t *prev = NULL;
    
    // Находим последний процесс в списке (ограничиваем итерации)
    while (p->next && p->next != start && iterations++ < MAX_ITERATIONS) {
        prev = p;
        p = p->next;
    }
    
    // Проверка на превышение лимита итераций
    if (iterations >= MAX_ITERATIONS) {
        printf("[PROCESS] WARNING: Possible corrupted process list detected in process_reap!\n");
        irq_enable();
        return;
    }
    
    iterations = 0;  // Сбрасываем счётчик для основного цикла
    
    // Сохраняем следующий процесс перед возможным удалением
    process_t *next_process = NULL;
    
    do {
        // Защита от бесконечного цикла
        if (iterations++ >= MAX_ITERATIONS) {
            printf("[PROCESS] WARNING: Infinite loop prevented in process_reap main loop!\n");
            break;
        }
        
        // Проверка на валидность указателя
        if (p == NULL) {
            printf("[PROCESS] ERROR: NULL process pointer in list!\n");
            break;
        }
        
        // Сохраняем следующий процесс ДО возможного удаления текущего
        next_process = p->next;
        
        // Проверка на зацикливание (указатель на самого себя)
        if (p == next_process && p != process_list) {
            printf("[PROCESS] ERROR: Process points to itself, list corrupted!\n");
            break;
        }
        
        if (p->state == PROCESS_TERMINATED && p != idle_process) {
            printf("[PROCESS] Reaping zombie PID %u ('%s')\n", p->pid, p->name);
            
            // Удаляем из списка
            if (p == process_list) {
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
                    // Находим предыдущий (также с защитой от бесконечного цикла)
                    process_t *last = process_list;
                    uint32_t find_iter = 0;
                    const uint32_t MAX_FIND = 10000;
                    
                    while (last->next != process_list && find_iter++ < MAX_FIND && last != NULL) {
                        last = last->next;
                    }
                    
                    if (find_iter >= MAX_FIND || last == NULL) {
                        printf("[PROCESS] ERROR: Failed to find last element, list corrupted!\n");
                        break;
                    }
                    
                    process_list = p->next;
                    last->next = process_list;
                }
            } else {
                // Удаляем из середины/конца
                process_t *prev_node = process_list;
                uint32_t find_iter = 0;
                const uint32_t MAX_FIND = 10000;
                int found = 0;
                
                // Защита от бесконечного цикла при поиске предыдущего узла
                while (prev_node != NULL && find_iter++ < MAX_FIND) {
                    if (prev_node->next == p) {
                        found = 1;
                        break;
                    }
                    prev_node = prev_node->next;
                    
                    // Защита от зацикливания
                    if (prev_node == process_list) {
                        break;
                    }
                }
                
                if (find_iter >= MAX_FIND || !found || prev_node == NULL) {
                    printf("[PROCESS] ERROR: Failed to find previous node, list corrupted!\n");
                    break;
                }
                
                prev_node->next = p->next;
            }
            
            // Освобождаем память процесса
            kfree(p);
        }
        
        // Переходим к следующему процессу
        p = next_process;
        
        // Проверка на зацикливание списка
        if (p == start && iterations > 1) {
            break;  // Вернулись к началу списка
        }
        
        // Защита от зацикливания если следующий процесс указывает на себя
        if (p != NULL && p->next == p && p != idle_process && p->state == PROCESS_TERMINATED) {
            // Удаляем такой процесс в следующей итерации
            continue;
        }
        
    } while (p != start && p != NULL && iterations < MAX_ITERATIONS);
    
    // Дополнительная проверка на случай повреждённого списка
    if (iterations >= MAX_ITERATIONS) {
        printf("[PROCESS] CRITICAL: Process list appears corrupted, re-initializing...\n");
        // Восстанавливаем список до безопасного состояния
        if (idle_process) {
            // Восстанавливаем idle процесс в безопасное состояние
            idle_process->next = idle_process;
            idle_process->state = PROCESS_READY;
            process_list = idle_process;
            current_process = idle_process;
        } else {
            process_list = NULL;
            current_process = NULL;
        }
    }
    
    // НЕ разрешаем прерывания здесь - это сделает вызывающая функция
    // irq_enable() будет вызван в schedule() после switch_to_process()
}

void schedule(void) {
    uint64_t rflags;
    asm volatile("pushfq; pop %0" : "=r"(rflags));
    if (!(rflags & 0x200)) {
        return;
    }
    
    if (!current_process) {
        return;
    }
    
    // Запрещаем прерывания для атомарной работы со списком процессов
    irq_disable();
    
    // Очищаем зомби-процессы (прерывания остаются запрещёнными)
    process_reap();
    
    process_t *next = current_process->next;
    int tries = 0;
    const int MAX_TRIES = 100;
    
    // Безопасный поиск следующего процесса с защитой от повреждённого списка
    while (next && (next->state == PROCESS_TERMINATED) && tries < MAX_TRIES) {
        next = next->next;
        tries++;
        
        // Защита от зацикливания
        if (next == current_process) {
            break;
        }
    }
    
    if (!next || next->state == PROCESS_TERMINATED || tries >= MAX_TRIES) {
        if (current_process != idle_process && idle_process != NULL) {
            next = idle_process;
        } else {
            irq_enable();  // Разрешаем прерывания перед выходом
            return;
        }
    }
    
    if (next == current_process) {
        irq_enable();  // Разрешаем прерывания перед выходом
        return;
    }
    
    // Устанавливаем RSP0 для нового процесса
    if (next->ring0_stack > 0) {
        tss_set_rsp0(next->ring0_stack);
    }
    
    // Переключаем контекст (прерывания остаются запрещёнными)
    // switch_to_process вызовет context_switch, который восстановит rflags
    // из контекста нового процесса, включая состояние прерываний
    switch_to_process(next);
    
    // ВАЖНО: после возвращения из switch_to_process (когда этот процесс
    // будет снова запланирован) мы должны разрешить прерывания, если
    // они были запрещены
    
    // Симметрично разрешаем прерывания
    irq_enable();
}

void switch_to_process(process_t *next) {
    if (!current_process || !next) return;
    if (current_process == next) return;
    
    process_t *prev = current_process;
    
    if (prev->state == PROCESS_RUNNING) prev->state = PROCESS_READY;
    next->state = PROCESS_RUNNING;
    
    current_process = next;
    
    context_switch(&prev->context, &next->context);
}