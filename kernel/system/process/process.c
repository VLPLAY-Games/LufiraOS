#include "process.h"
#include "system/mm/heap.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"

// Добавляем реализацию memset
static void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

// Связанный список процессов
static process_t *process_list = NULL;
process_t *current_process = NULL;

// Счётчик PID'ов
static uint32_t next_pid = 1;

// Холостой процесс (когда нет других задач)
static process_t *idle_process = NULL;

// Счётчик переключений (для отладки)
static uint64_t switch_count = 0;

// Функция холостого процесса
static void idle_thread(void) {
    while (1) {
        asm volatile("hlt");
    }
}

void process_init(void) {
    // Создаём холостой процесс
    idle_process = (process_t*)kmalloc(sizeof(process_t));
    if (!idle_process) {
        printf("[PROCESS] Failed to allocate idle process\n");
        return;
    }
    
    // Инициализируем его
    idle_process->pid = 0;
    idle_process->state = PROCESS_READY;
    idle_process->stack_base = 0;
    idle_process->stack_size = 0;
    idle_process->page_table = 0;
    idle_process->next = NULL;
    
    // Копируем имя
    const char *name = "idle";
    for (int i = 0; i < 31 && name[i]; i++) {
        idle_process->name[i] = name[i];
    }
    idle_process->name[31] = '\0';
    
    // Сохраняем текущий контекст как контекст idle
    asm volatile("mov %%cr3, %0" : "=r"(idle_process->context.cr3));
    
    process_list = idle_process;
    current_process = idle_process;
    
    printf("[PROCESS] Process manager initialized\n");
}

static uint64_t allocate_kernel_stack(size_t size) {
    // Выделяем физические страницы для стека
    size_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    // Выделяем первую страницу
    uint64_t first_phys = pmm_alloc_page();
    if (!first_phys) {
        printf("[PROCESS] Failed to allocate first stack page\n");
        return 0;
    }
    
    // Выделяем остальные страницы
    for (size_t i = 1; i < num_pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            printf("[PROCESS] Failed to allocate stack page %zu\n", i);
            for (size_t j = 0; j < i; j++) {
                pmm_free_page(first_phys + j * PAGE_SIZE);
            }
            return 0;
        }
        
        // Проверяем, что страницы последовательные
        if (phys != first_phys + i * PAGE_SIZE) {
            printf("[PROCESS] Non-contiguous pages, freeing and retrying\n");
            pmm_free_page(phys);
            for (size_t j = 0; j < i; j++) {
                pmm_free_page(first_phys + j * PAGE_SIZE);
            }
            return 0;
        }
    }
    
    // Возвращаем вершину стека
    uint64_t stack_top = first_phys + num_pages * PAGE_SIZE;
    return stack_top;
}

process_t* process_create(const char *name, void (*entry)(void)) {
    process_t *proc = (process_t*)kmalloc(sizeof(process_t));
    if (!proc) {
        printf("[PROCESS] Failed to allocate process structure\n");
        return NULL;
    }
    
    // Заполняем базовую информацию
    proc->pid = next_pid++;
    proc->state = PROCESS_READY;
    
    // Копируем имя
    for (int i = 0; i < 31 && name[i]; i++) {
        proc->name[i] = name[i];
    }
    proc->name[31] = '\0';
    
    // Выделяем стек (16 КБ = 4 страницы)
    proc->stack_size = 16384;
    proc->stack_base = allocate_kernel_stack(proc->stack_size);
    if (!proc->stack_base) {
        printf("[PROCESS] Failed to allocate stack for process %s\n", name);
        kfree(proc);
        return NULL;
    }
    
    // Используем текущую таблицу страниц (identity mapping)
    asm volatile("mov %%cr3, %0" : "=r"(proc->page_table));
    
    // Инициализируем контекст нулями
    memset(&proc->context, 0, sizeof(process_context_t));
    
    // Настраиваем стек
    uint64_t rsp = proc->stack_base;
    
    // Создаём фрейм стека как будто нас прервали
    // Стек растёт вниз, поэтому отнимаем
    rsp -= 8;  // Место для возврата из process_exit
    uint64_t *stack = (uint64_t*)rsp;
    *stack = (uint64_t)process_exit;
    
    rsp -= 8;  // RIP который заберёт ret в context_switch
    stack = (uint64_t*)rsp;
    *stack = (uint64_t)entry;
    
    rsp -= 8;  // Дополнительное выравнивание
    rsp -= 8;  // RFLAGS
    stack = (uint64_t*)rsp;
    *stack = 0x202;  // IF flag set
    
    // Настраиваем контекст
    proc->context.rsp = rsp;
    proc->context.rip = (uint64_t)entry;
    proc->context.rflags = 0x202;
    proc->context.cr3 = proc->page_table;
    
    // Добавляем в список процессов (в КОНЕЦ)
    proc->next = NULL;
    
    if (!process_list) {
        process_list = proc;
        proc->next = proc;  // Зацикливаем на себя
    } else {
        process_t *last = process_list;
        // Ищем последний процесс
        while (last->next && last->next != process_list) {
            last = last->next;
        }
        last->next = proc;
        proc->next = process_list;  // Зацикливаем список
    }
    
    printf("[PROCESS] Created process '%s' (PID %u, stack: 0x%lx-0x%lx)\n", 
           name, proc->pid, proc->stack_base - proc->stack_size, proc->stack_base);
    
    return proc;
}

void process_exit(void) {
    printf("\n[PROCESS] Process %u ('%s') exiting\n", 
           current_process->pid, current_process->name);
    
    asm volatile("cli");
    
    // Помечаем как завершённый
    current_process->state = PROCESS_TERMINATED;
    
    // Освобождаем стек
    if (current_process->stack_base > 0 && current_process->stack_size > 0) {
        uint64_t stack_start = current_process->stack_base - current_process->stack_size;
        size_t num_pages = current_process->stack_size / PAGE_SIZE;
        for (size_t i = 0; i < num_pages; i++) {
            pmm_free_page(stack_start + i * PAGE_SIZE);
        }
    }
    
    asm volatile("sti");
    
    // Больше никогда не возвращаемся
    schedule();
    
    while (1) {
        asm volatile("hlt");
    }
}

void schedule(void) {
    // Не переключаемся если прерывания запрещены
    uint64_t rflags;
    asm volatile("pushfq; pop %0" : "=r"(rflags));
    if (!(rflags & 0x200)) {
        return;  // Interrupts disabled, don't switch
    }
    
    if (!current_process) {
        return;
    }
    
    process_t *next = current_process->next;
    
    // Пропускаем завершённые процессы
    int tries = 0;
    while (next && next->state == PROCESS_TERMINATED && tries < 100) {
        next = next->next;
        tries++;
    }
    
    // Если все процессы завершены
    if (!next || next->state == PROCESS_TERMINATED) {
        if (current_process != idle_process) {
            next = idle_process;
        } else {
            return;
        }
    }
    
    // Не переключаемся на тот же процесс
    if (next == current_process) {
        return;
    }
    
    switch_count++;
    
    // Переключаем контекст
    switch_to_process(next);
}

void switch_to_process(process_t *next) {
    if (!current_process || !next) {
        return;
    }
    
    if (current_process == next) {
        return;
    }
    
    process_t *prev = current_process;
    
    // Обновляем состояния
    if (prev->state == PROCESS_RUNNING) {
        prev->state = PROCESS_READY;
    }
    next->state = PROCESS_RUNNING;
    
    // Устанавливаем новый текущий процесс
    current_process = next;
    
    // Переключаем контекст
    context_switch(&prev->context, &next->context);
}