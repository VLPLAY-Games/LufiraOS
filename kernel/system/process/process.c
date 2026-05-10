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
    
    // Нам нужны последовательные виртуальные адреса
    // Для простоты выделим физические страницы и будем использовать
    // их напрямую через identity mapping (первые 2MB уже отображены 1:1)
    
    // Выделяем первую страницу
    uint64_t first_phys = pmm_alloc_page();
    if (!first_phys) {
        printf("[PROCESS] Failed to allocate first stack page\n");
        return 0;
    }
    
    // Выделяем остальные страницы (они могут быть не последовательными)
    for (size_t i = 1; i < num_pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            printf("[PROCESS] Failed to allocate stack page %zu\n", i);
            // Освобождаем уже выделенные
            for (size_t j = 0; j < i; j++) {
                pmm_free_page(first_phys + j * PAGE_SIZE);
            }
            return 0;
        }
        
        // Проверяем, что страницы последовательные
        if (phys != first_phys + i * PAGE_SIZE) {
            printf("[PROCESS] Non-contiguous pages, freeing and retrying\n");
            pmm_free_page(phys);
            // Освобождаем всё и возвращаем ошибку
            for (size_t j = 0; j < i; j++) {
                pmm_free_page(first_phys + j * PAGE_SIZE);
            }
            return 0;
        }
    }
    
    // Возвращаем вершину стека (старший адрес)
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
    // Стек растёт вниз, RSP должен указывать на вершину стека
    // Оставляем место для адреса возврата
    uint64_t rsp = proc->stack_base - 8;
    
    // Записываем адрес возврата (process_exit) на вершину стека
    uint64_t *stack_ptr = (uint64_t*)rsp;
    *stack_ptr = (uint64_t)process_exit;
    
    // Настраиваем контекст
    proc->context.rsp = rsp;
    proc->context.rip = (uint64_t)entry;
    proc->context.rflags = 0x202;  // IF (interrupt flag) + reserved bit
    proc->context.cr3 = proc->page_table;
    
    // Добавляем в список процессов
    proc->next = NULL;
    
    if (!process_list) {
        process_list = proc;
    } else {
        process_t *last = process_list;
        while (last->next) {
            last = last->next;
        }
        last->next = proc;
    }
    
    printf("[PROCESS] Created process '%s' (PID %u, stack: 0x%lx-0x%lx)\n", 
           name, proc->pid, proc->stack_base - proc->stack_size, proc->stack_base);
    
    return proc;
}

void process_exit(void) {
    printf("\n[PROCESS] Process %u ('%s') exiting\n", 
           current_process->pid, current_process->name);
    
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
    
    // Планировщик больше не вернёт этот процесс
    schedule();
    
    // Сюда мы никогда не попадём
    while (1) {
        asm volatile("hlt");
    }
}

void schedule(void) {
    if (!current_process) {
        return;
    }
    
    // Находим следующий процесс для запуска
    process_t *next = current_process->next;
    
    // Если это конец списка или процесс завершён — начинаем с начала
    if (!next) {
        next = process_list;
    }
    
    // Пропускаем завершённые процессы
    while (next && next->state == PROCESS_TERMINATED) {
        next = next->next;
        if (!next) {
            next = process_list;
            // Если вернулись к началу и все завершены - остаёмся на idle
            if (next == current_process) {
                break;
            }
        }
    }
    
    // Если нет готовых процессов - используем idle
    if (!next || next == current_process) {
        if (current_process != idle_process) {
            next = idle_process;
        } else {
            return;
        }
    }
    
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
    
    // Выполняем переключение контекста
    context_switch(&prev->context, &next->context);
}