#include "process.h"
#include "system/timer/pit.h"
#include "system/mm/heap.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "system/cpu/gdt.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"

#ifndef PAGE_PS
#define PAGE_PS 0x80    // Page size (2MB/1GB) — как и в elf.c
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

process_t *process_list = NULL;
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

// Настоящий глубокий клон адресного пространства для fork(): ядерная
// половина (индексы PML4 256-511) заводится штатно, как для любого нового
// процесса (create_address_space(kernel_cr3) + sync_kernel_mappings) —
// она в любом случае общая для всех процессов. Пользовательская половина
// (индексы 0-255) рекурсивно обходится по src_pml4_phys, и КАЖДАЯ занятая
// страница данных копируется в новую физическую страницу (eager copy, без
// copy-on-write — в paging.c сейчас нет инфраструктуры под COW).
//
// При нехватке памяти на каком-либо уровне возвращает 0; уже выделенные
// до этого момента страницы частично построенного дерева намеренно не
// освобождаются — как и во всей остальной работе с адресными
// пространствами в этом файле (см. process_reap()), полного разбора PML4
// здесь пока нет, а fork() при нехватке памяти — редкий крайний случай.
static uint64_t clone_address_space_deep(uint64_t src_pml4_phys) {
    uint64_t new_pml4_phys = create_address_space(kernel_cr3);
    if (!new_pml4_phys)
        return 0;

    uint64_t current_kernel_pml4;
    asm volatile("mov %%cr3, %0" : "=r"(current_kernel_pml4));
    sync_kernel_mappings(new_pml4_phys, current_kernel_pml4);

    uint64_t *src_pml4 = (uint64_t*)src_pml4_phys;
    uint64_t *dst_pml4 = (uint64_t*)new_pml4_phys;

    for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
        if (!(src_pml4[pml4_idx] & PAGE_PRESENT))
            continue;

        uint64_t new_pdpt_phys = pmm_alloc_page();
        if (!new_pdpt_phys) return 0;
        uint64_t *new_pdpt = (uint64_t*)new_pdpt_phys;
        for (int i = 0; i < 512; i++) new_pdpt[i] = 0;

        uint64_t *src_pdpt = (uint64_t*)(src_pml4[pml4_idx] & ~0xFFFULL);

        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            if (!(src_pdpt[pdpt_idx] & PAGE_PRESENT))
                continue;

            uint64_t new_pd_phys = pmm_alloc_page();
            if (!new_pd_phys) return 0;
            uint64_t *new_pd = (uint64_t*)new_pd_phys;
            for (int i = 0; i < 512; i++) new_pd[i] = 0;

            uint64_t *src_pd = (uint64_t*)(src_pdpt[pdpt_idx] & ~0xFFFULL);

            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                if (!(src_pd[pd_idx] & PAGE_PRESENT))
                    continue;

                if (src_pd[pd_idx] & PAGE_PS) {
                    // 2MB/1GB user-страницы в этой кодовой базе не
                    // создаются (map_page_in_space всегда работает по
                    // 4KB) — на всякий случай просто пропускаем.
                    continue;
                }

                uint64_t new_pt_phys = pmm_alloc_page();
                if (!new_pt_phys) return 0;
                uint64_t *new_pt = (uint64_t*)new_pt_phys;
                for (int i = 0; i < 512; i++) new_pt[i] = 0;

                uint64_t *src_pt = (uint64_t*)(src_pd[pd_idx] & ~0xFFFULL);

                for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                    if (!(src_pt[pt_idx] & PAGE_PRESENT))
                        continue;

                    uint64_t src_phys = src_pt[pt_idx] & ~0xFFFULL;
                    uint64_t flags = src_pt[pt_idx] & (0xFFFULL | PAGE_NX);

                    uint64_t new_phys = pmm_alloc_page();
                    if (!new_phys) return 0;

                    // Все физические адреса в этом ядре доступны через
                    // identity mapping, поэтому можно копировать напрямую.
                    memcpy((void*)new_phys, (void*)src_phys, PAGE_SIZE);

                    new_pt[pt_idx] = (new_phys & ~0xFFFULL) | flags;
                }

                new_pd[pd_idx] = (new_pt_phys & ~0xFFFULL) |
                                 (src_pd[pd_idx] & 0xFFFULL);
            }

            new_pdpt[pdpt_idx] = (new_pd_phys & ~0xFFFULL) |
                                 (src_pdpt[pdpt_idx] & 0xFFFULL);
        }

        dst_pml4[pml4_idx] = (new_pdpt_phys & ~0xFFFULL) |
                             (src_pml4[pml4_idx] & 0xFFFULL);
    }

    return new_pml4_phys;
}

void process_init(void) {
    // Сохраняем корневой ядерный PML4 на раннем этапе (до загрузки процессов)
    asm volatile("mov %%cr3, %0" : "=r"(kernel_cr3));
    
    idle_process = (process_t*)kmalloc(sizeof(process_t));
    if (!idle_process) return;
    
    idle_process->pid = 0;
    idle_process->ppid = 0;
    idle_process->exit_code = 0;
    idle_process->wait_target_pid = 0;
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

    // fd-таблица idle-процесса: пока пустая (memset), содержимым (stdio)
    // её заполнит vfs_init(), который выполняется позже при загрузке.
    memset(&idle_process->fd_table, 0, sizeof(fd_table_t));
    current_fd_table = &idle_process->fd_table;

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
    proc->ppid = current_process ? current_process->pid : 0;
    proc->exit_code = 0;
    proc->wait_target_pid = 0;
    proc->state = PROCESS_READY;
    
    for (int i = 0; i < 31 && name[i]; i++) proc->name[i] = name[i];
    proc->name[31] = '\0';

    // Собственная fd-таблица процесса со свежими stdin/stdout/stderr
    // (консоль). Реального наследования fd родителя здесь нет — это
    // подходящее поведение для run/runbg; fork() (см. process_fork())
    // отдельно дублирует именно РОДИТЕЛЬСКУЮ таблицу поверх этой.
    vfs_init_fd_table(&proc->fd_table);

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

// Ищет процесс, который сейчас заблокирован в process_wait(), ожидая
// именно child (по pid или WAIT_ANY_PID). Возвращает NULL, если такого нет.
static process_t *find_waiting_parent(process_t *child) {
    if (!child->ppid || !process_list)
        return NULL;

    process_t *p = process_list;
    process_t *start = p;

    do {
        if (p->pid == child->ppid &&
            p->state == PROCESS_BLOCKED &&
            (p->wait_target_pid == child->pid ||
             p->wait_target_pid == WAIT_ANY_PID))
        {
            return p;
        }
        p = p->next;
    } while (p && p != start);

    return NULL;
}

// Будит родителя child'а, если тот сейчас ждёт его в process_wait() —
// используется и нормальным завершением (process_exit), и process_kill().
// Возвращает разбуженного родителя (уже переведённого в READY) либо NULL.
static process_t *wake_waiting_parent(process_t *child) {
    process_t *waiter = find_waiting_parent(child);
    if (!waiter)
        return NULL;

    waiter->wait_target_pid = 0;
    waiter->state = PROCESS_READY;
    return waiter;
}

// Убирает target (уже PROCESS_TERMINATED, найденный process_wait()) из
// кольцевого списка процессов и освобождает его process_t. Как и
// process_reap(), не освобождает page_table/ring0-стек — в кодовой базе
// пока нет функции разбора PML4 целиком.
static void process_remove_from_list(process_t *target) {
    if (!process_list || !target)
        return;

    if (target->next == target) {
        if (process_list == target)
            process_list = NULL;
        kfree(target);
        return;
    }

    if (process_list == target) {
        process_t *last = target;
        while (last->next != target)
            last = last->next;
        process_list = target->next;
        last->next = process_list;
    } else {
        process_t *prev = process_list;
        while (prev->next != target && prev->next != process_list)
            prev = prev->next;
        if (prev->next == target)
            prev->next = target->next;
    }

    kfree(target);
}

// Немедленно убирает недостроенный процесс из списка планировщика (см.
// process.h) — например, если process_fork() не смог доделать ребёнка.
void process_discard(process_t *proc) {
    if (!proc)
        return;

    proc->state = PROCESS_TERMINATED;
    process_remove_from_list(proc);
}

void process_exit(int exit_code) {
    process_t *exiting_process = current_process;

    if (!exiting_process) {
        while (1)
            asm volatile("hlt");
    }

    printf("\n[PROCESS] Process %u ('%s') exiting (code %d)\n",
           exiting_process->pid,
           exiting_process->name,
           exit_code);

    exiting_process->exit_code = exit_code;
    exiting_process->state = PROCESS_TERMINATED;

    process_t *waiter = wake_waiting_parent(exiting_process);
    if (waiter) {
        switch_to_process(waiter);
    } else {
        schedule();
    }

    /*
     * Если сюда вернулись — что-то пошло не так.
     */
    while (1)
        asm volatile("hlt");
}

// Ждёт завершения ребёнка текущего процесса. См. комментарий в process.h.
int process_wait(uint32_t pid, int *status_out) {
    if (!current_process)
        return -1;

    uint32_t caller_pid = current_process->pid;
    uint32_t target = (pid == 0) ? WAIT_ANY_PID : pid;

    for (;;) {
        process_t *zombie = NULL;
        int has_child = 0;

        if (process_list) {
            process_t *p = process_list;
            process_t *start = p;

            do {
                if (p->ppid == caller_pid &&
                    (target == WAIT_ANY_PID || p->pid == target))
                {
                    has_child = 1;
                    if (p->state == PROCESS_TERMINATED) {
                        zombie = p;
                        break;
                    }
                }
                p = p->next;
            } while (p && p != start);
        }

        if (zombie) {
            uint32_t zpid = zombie->pid;
            int code = zombie->exit_code;

            process_remove_from_list(zombie);

            if (status_out)
                *status_out = code;

            return (int)zpid;
        }

        if (!has_child) {
            // У вызывающего нет (или больше нет) такого ребёнка.
            return -1;
        }

        // Ребёнок жив — блокируемся до его завершения.
        current_process->wait_target_pid = target;
        current_process->state = PROCESS_BLOCKED;

        schedule();

        // Возобновились: либо нас разбудил exit подходящего ребёнка,
        // либо это ложное пробуждение — в обоих случаях просто заново
        // сканируем список выше.
        current_process->wait_target_pid = 0;
    }
}

// Готовит НОВОЕ адресное пространство и пользовательский стек для exec(),
// не трогая ничего в уже работающем процессе proc. Если памяти не хватило,
// ничего не остаётся привязанным к proc — старый образ процесса цел и
// вызывающий код может просто сообщить об ошибке и продолжить работу.
int process_prepare_exec(process_t *proc,
                          uint64_t *new_pml4_out,
                          uint64_t *new_stack_out)
{
    if (!proc || !new_pml4_out || !new_stack_out)
        return -1;

    uint64_t new_pml4 = create_address_space(kernel_cr3);
    if (!new_pml4)
        return -1;

    uint64_t current_kernel_pml4;
    asm volatile("mov %%cr3, %0" : "=r"(current_kernel_pml4));
    sync_kernel_mappings(new_pml4, current_kernel_pml4);

    uint64_t new_stack = allocate_user_stack(USER_STACK_SIZE, new_pml4, proc->pid);
    if (!new_stack) {
        pmm_free_page(new_pml4);
        return -1;
    }

    *new_pml4_out = new_pml4;
    *new_stack_out = new_stack;
    return 0;
}

// Подтверждает exec(): переключает proc на уже подготовленные (и
// заполненные загруженным ELF) адресное пространство и стек. PID,
// ring0-стек и позиция в списке планировщика не меняются — это замена
// образа процесса на месте, а не создание нового процесса.
//
// Старое адресное пространство/стек proc намеренно не освобождаются:
// в кодовой базе пока нет функции разбора/освобождения PML4 целиком
// (process_reap() точно так же не освобождает page_table завершённых
// процессов), так что делать это только здесь было бы половинчатым
// решением.
int process_commit_exec(process_t *proc,
                        uint64_t new_pml4,
                        uint64_t new_stack,
                        const char *name)
{
    if (!proc)
        return -1;

    proc->page_table = new_pml4;
    proc->stack_base = new_stack;
    proc->stack_size = USER_STACK_SIZE;

    for (int i = 0; i < 31 && name[i]; i++) proc->name[i] = name[i];
    proc->name[31] = '\0';

    return 0;
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

void process_sleep(uint64_t milliseconds) {
    if (!current_process || milliseconds == 0)
        return;

    uint64_t ticks = (milliseconds + 9) / 10;

    current_process->wakeup_tick = pit_get_ticks() + ticks;
    current_process->state = PROCESS_SLEEPING;

    schedule();
}

void schedule(void) {
    /*
     * Scheduler должен уметь работать и при IF=0.
     *
     * Это особенно важно для process_exit(), syscall handler
     * и interrupt context.
     */

    irq_disable();

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
        if (idle_process && idle_process != current_process) {
            next = idle_process;
        } else {
            irq_enable();
            return;
        }
    }

    if (next == current_process) {
        irq_enable();
        return;
    }

    switch_to_process(next);

    /*
     * В нормальном случае после switch_to_process()
     * этот код не выполняется сразу.
     *
     * Он продолжится тогда, когда старый процесс
     * будет снова выбран scheduler'ом.
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
    current_fd_table = &next->fd_table;

    process_context_t *prev_context = (prev && prev != next) ? &prev->context : &idle_process->context;

    current_process = next;
    current_kernel_rsp = next->ring0_stack;
    
    context_switch(prev_context, &next->context);
}

static const char *process_state_name(process_state_t state)
{
    switch (state) {
        case PROCESS_READY:
            return "READY";

        case PROCESS_RUNNING:
            return "RUNNING";

        case PROCESS_BLOCKED:
            return "BLOCKED";

        case PROCESS_SLEEPING:
            return "SLEEPING";

        case PROCESS_TERMINATED:
            return "TERMINATED";

        default:
            return "UNKNOWN";
    }
}

void process_ps(void)
{
    irq_disable();

    printf("\n");
    printf("PID   STATE       NAME\n");
    printf("-------------------------------\n");

    if (!process_list) {
        printf("No processes\n");
        irq_enable();
        return;
    }

    process_t *p = process_list;
    process_t *start = p;

    do {
        printf("%u     %s     %s\n",
               p->pid,
               process_state_name(p->state),
               p->name);

        p = p->next;
    } while (p && p != start);

    printf("\n");

    irq_enable();
}

int process_kill(uint32_t pid)
{
    if (!process_list)
        return -1;

    process_t *p = process_list;

    do {
        if (p->pid == pid) {

            if (p == idle_process)
                return -1;

            if (p->state == PROCESS_TERMINATED)
                return 0;

            printf("[PROCESS] Killing PID %u ('%s')\n",
                   p->pid, p->name);

            p->exit_code = -1;
            p->state = PROCESS_TERMINATED;

            process_t *waiter = wake_waiting_parent(p);

            if (p == current_process) {
                if (waiter) {
                    switch_to_process(waiter);
                } else {
                    schedule();
                }

                while (1)
                    asm volatile("hlt");
            }

            return 0;
        }

        p = p->next;

    } while (p && p != process_list);

    return -1;
}

// Раскладка кадра регистров, который syscall_entry.S сохраняет на
// ядерном стеке перед вызовом syscall_handler() (см. комментарии в этом
// файле). Указатель на начало этого кадра передаётся syscall_handler()
// 7-м аргументом — только SYS_FORK его использует.
typedef struct __attribute__((packed)) {
    uint64_t r9;
    uint64_t r8;
    uint64_t r10;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rax;      // номер syscall на входе
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rflags;   // r11 на входе в syscall
    uint64_t rip;      // rcx на входе в syscall — адрес возврата в user-коде
} syscall_frame_t;

extern uint64_t user_rsp_save;

// Настоящий fork(). См. подробное описание в process.h.
uint64_t process_fork(uint64_t frame_ptr) {
    process_t *parent = current_process;
    if (!parent || !frame_ptr)
        return (uint64_t)-1;

    syscall_frame_t *frame = (syscall_frame_t *)frame_ptr;

    process_t *child = process_create(parent->name, NULL);
    if (!child)
        return (uint64_t)-1;

    // process_create() уже выделил ребёнку "пустое" адресное пространство
    // и собственный (незанятый) пользовательский стек по АДРЕСУ, вычисленному
    // из pid ребёнка. Для fork() нам нужно совсем другое — точная копия
    // адресного пространства РОДИТЕЛЯ (включая его виртуальный адрес
    // стека), так что этот плейсхолдер просто заменяется ниже.
    uint64_t placeholder_pml4 = child->page_table;

    uint64_t new_pml4 = clone_address_space_deep(parent->page_table);
    if (!new_pml4) {
        printf("[FORK] Not enough memory to clone address space\n");
        process_discard(child);
        return (uint64_t)-1;
    }

    child->page_table = new_pml4;
    pmm_free_page(placeholder_pml4);

    // Ребёнок наследует ТОТ ЖЕ виртуальный адрес стека, что и родитель —
    // он был скопирован вместе со всем остальным пользовательским
    // адресным пространством выше.
    child->stack_base = parent->stack_base;
    child->stack_size = parent->stack_size;

    // fd-таблица: родитель и ребёнок получают собственные fd, но
    // указывающие на ОДНИ И ТЕ ЖЕ открытые file_t/inode — как и должно
    // быть у настоящего fork().
    child->fd_table = parent->fd_table;
    for (int i = 0; i < MAX_FD_PER_PROCESS; i++) {
        file_t *f = child->fd_table.files[i];
        if (f && f->inode) {
            f->inode->ref_count++;
        }
    }

    // Контекст ребёнка продолжает выполнение СРАЗУ ПОСЛЕ инструкции
    // syscall в родителе — тот же rip/rflags и те же callee-saved регистры
    // (rbx/rbp/r12-r15, которые компилятор родителя рассчитывает получить
    // в неизменном виде после возврата из "обёртки" fork()), но с rax = 0
    // — это и есть возвращаемое значение fork() для ребёнка.
    //
    // Одно отличие от родителя: родитель вернётся в user mode штатным
    // iretq в syscall_entry.S (ring 3), а ребёнок стартует через обычный
    // context_switch()/jmp, как и любой другой новый процесс в этом ядре,
    // и поэтому первое время выполняется в ring 0 — точно так же, как
    // процесс, созданный через elf_exec_background(), до своего первого
    // собственного системного вызова.
    process_context_t *ctx = &child->context;
    memset(ctx, 0, sizeof(*ctx));
    ctx->rip = frame->rip;
    ctx->rsp = user_rsp_save;
    ctx->rflags = frame->rflags;
    ctx->rbx = frame->rbx;
    ctx->rbp = frame->rbp;
    ctx->r12 = frame->r12;
    ctx->r13 = frame->r13;
    ctx->r14 = frame->r14;
    ctx->r15 = frame->r15;
    ctx->rax = 0;
    ctx->cr3 = new_pml4;

    child->state = PROCESS_READY;

    printf("[FORK] PID %u forked into PID %u\n", parent->pid, child->pid);

    return (uint64_t)child->pid;
}