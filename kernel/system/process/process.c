#include "process.h"
#include "system/mm/heap.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
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

static void idle_thread(void) {
    while (1) __asm__("hlt");
}

// Копирование kernel space во все новые PML4
static uint64_t create_address_space(uint64_t kernel_pml4_phys) {
    uint64_t new_pml4_phys = pmm_alloc_page();
    if (!new_pml4_phys) return 0;
    
    uint64_t *new_pml4 = (uint64_t*)new_pml4_phys;
    uint64_t *kernel_pml4 = (uint64_t*)kernel_pml4_phys;
    
    // Копируем ВСЕ записи из ядерного PML4
    // Это даст процессу доступ к identity mapped памяти (включая ядро, стеки, фреймбуфер)
    for (int i = 0; i < 512; i++) {
        new_pml4[i] = kernel_pml4[i];
    }
    
    return new_pml4_phys;
}

void process_init(void) {
    idle_process = (process_t*)kmalloc(sizeof(process_t));
    if (!idle_process) return;
    
    idle_process->pid = 0;
    idle_process->state = PROCESS_READY;
    idle_process->stack_base = 0;
    idle_process->stack_size = 0;
    idle_process->next = NULL;
    
    const char *name = "idle";
    for (int i = 0; i < 31 && name[i]; i++) idle_process->name[i] = name[i];
    idle_process->name[31] = '\0';
    
    asm volatile("mov %%cr3, %0" : "=r"(idle_process->page_table));
    
    process_list = idle_process;
    current_process = idle_process;
    
    printf("[PROCESS] Process manager initialized\n");
}

static uint64_t allocate_stack(size_t size, uint64_t pml4_phys) {
    size_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    // Временно переключаемся на адресное пространство процесса
    uint64_t old_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(old_cr3));
    asm volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
    
    uint64_t first_phys = pmm_alloc_page();
    if (!first_phys) {
        asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
        return 0;
    }
    
    for (size_t i = 1; i < num_pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            for (size_t j = 0; j < i; j++) {
                pmm_free_page(first_phys + j * PAGE_SIZE);
            }
            asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
            return 0;
        }
        
        if (phys != first_phys + i * PAGE_SIZE) {
            pmm_free_page(phys);
            for (size_t j = 0; j < i; j++) {
                pmm_free_page(first_phys + j * PAGE_SIZE);
            }
            asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
            return 0;
        }
    }
    
    asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
    
    return first_phys + num_pages * PAGE_SIZE;
}

process_t* process_create(const char *name, void (*entry)(void)) {
    process_t *proc = (process_t*)kmalloc(sizeof(process_t));
    if (!proc) return NULL;
    
    proc->pid = next_pid++;
    proc->state = PROCESS_READY;
    
    for (int i = 0; i < 31 && name[i]; i++) proc->name[i] = name[i];
    proc->name[31] = '\0';
    
    // Получаем текущий PML4
    uint64_t kernel_pml4;
    asm volatile("mov %%cr3, %0" : "=r"(kernel_pml4));
    
    // Создаём копию адресного пространства
    uint64_t new_pml4 = create_address_space(kernel_pml4);
    if (!new_pml4) {
        kfree(proc);
        return NULL;
    }
    proc->page_table = new_pml4;
    
    // Выделяем стек в НОВОМ адресном пространстве
    proc->stack_size = 16384;
    proc->stack_base = allocate_stack(proc->stack_size, new_pml4);
    if (!proc->stack_base) {
        pmm_free_page(new_pml4);
        kfree(proc);
        return NULL;
    }
    
    // Инициализируем контекст
    memset(&proc->context, 0, sizeof(process_context_t));
    
    // Стек уже выделен, записываем в него через временное переключение
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
    
    rsp -= 16;
    stack_ptr = (uint64_t*)(rsp + 8);
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
    
    printf("[PROCESS] Created '%s' (PID %u, CR3: 0x%lx)\n", name, proc->pid, new_pml4);
    
    return proc;
}

void process_exit(void) {
    printf("\n[PROCESS] Process %u ('%s') exiting\n", 
           current_process->pid, current_process->name);
    
    asm volatile("cli");
    current_process->state = PROCESS_TERMINATED;
    
    if (current_process->stack_base > 0 && current_process->stack_size > 0) {
        uint64_t stack_start = current_process->stack_base - current_process->stack_size;
        size_t num_pages = current_process->stack_size / PAGE_SIZE;
        
        uint64_t old_cr3;
        asm volatile("mov %%cr3, %0" : "=r"(old_cr3));
        asm volatile("mov %0, %%cr3" : : "r"(current_process->page_table) : "memory");
        
        for (size_t i = 0; i < num_pages; i++) {
            pmm_free_page(stack_start + i * PAGE_SIZE);
        }
        
        asm volatile("mov %0, %%cr3" : : "r"(old_cr3) : "memory");
    }
    
    // Освобождаем PML4 процесса
    if (current_process->page_table != idle_process->page_table) {
        pmm_free_page(current_process->page_table);
    }
    
    asm volatile("sti");
    schedule();
    while (1) __asm__("hlt");
}

void schedule(void) {
    uint64_t rflags;
    asm volatile("pushfq; pop %0" : "=r"(rflags));
    if (!(rflags & 0x200)) return;
    
    if (!current_process) return;
    
    process_t *next = current_process->next;
    int tries = 0;
    
    while (next && next->state == PROCESS_TERMINATED && tries < 100) {
        next = next->next;
        tries++;
    }
    
    if (!next || next->state == PROCESS_TERMINATED) {
        if (current_process != idle_process) {
            next = idle_process;
        } else {
            return;
        }
    }
    
    if (next == current_process) return;
    
    switch_to_process(next);
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