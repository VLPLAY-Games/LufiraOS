#include "process.h"
#include "system/timer/pit.h"
#include "system/mm/heap.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "system/cpu/gdt.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"
#include "lib/string.h"

#ifndef PAGE_PS
#define PAGE_PS 0x80    // Page size (2MB/1GB) — как и в elf.c
#endif

process_t *process_list = NULL;
process_t *current_process = NULL;
uint64_t current_kernel_rsp = 0; // Глобальная переменная для asm
static uint32_t next_pid = 1;
static process_t *idle_process = NULL;
uint64_t kernel_cr3 = 0;

// Глобальный счётчик для вложенных запретов прерываний.
//
// irq_disable()/irq_enable() вызываются не только из обычного кода ядра,
// но и синхронно ИЗНУТРИ обработчиков IRQ (шелл выполняет команды прямо в
// keyboard_irq_handler() — см. комментарий в elf.c про exec), где
// прерывания уже аппаратно отключены самим CPU при входе в interrupt
// gate. Раньше irq_enable() при обнулении счётчика безусловно делал
// "sti" — если самый первый irq_disable() в цепочке вызовов на самом деле
// застал прерывания УЖЕ выключенными (мы внутри чужого IRQ), это
// ПРЕЖДЕВРЕМЕННО включало их посреди создания процесса, и вложенный
// таймерный IRQ (который теперь ещё и опрашивает USB через usb_poll())
// мог вклиниться прямо в этот момент. Поэтому запоминаем реальное
// состояние EFLAGS.IF на момент самого первого захвата и включаем
// прерывания обратно, только если они действительно были включены.
static volatile uint32_t irq_disable_counter = 0;
static uint64_t irq_saved_flags = 0;

// Вспомогательные функции для управления прерываниями
static inline void irq_disable(void) {
    if (irq_disable_counter == 0) {
        asm volatile("pushfq; popq %0" : "=r"(irq_saved_flags) :: "memory");
    }
    asm volatile("cli");
    irq_disable_counter++;
}

static inline void irq_enable(void) {
    if (irq_disable_counter > 0) {
        irq_disable_counter--;
        if (irq_disable_counter == 0 && (irq_saved_flags & (1u << 9))) {
            asm volatile("sti");
        }
    }
}

// Выделение Ring 0 стека.
//
// НЕ переключает CR3 — раньше эта функция временно переключалась на
// proc->page_table, чтобы иметь возможность вызвать обычный map_page()
// (который всегда работает с ТЕКУЩИМ CR3). Но весь этот вызов идёт вложенно
// из process_create(), которая, в свою очередь, обычно вызывается прямо из
// keyboard_irq_handler() — а значит текущий стек вызовов физически лежит
// на ПОЛЬЗОВАТЕЛЬСКОМ стеке ВЫЗЫВАЮЩЕГО процесса (shell и любой другой
// процесс в этом ядре выполняется на CPL0, но на СВОЁМ user-стеке, пока не
// сделает первый syscall). Индекс PML4, покрывающий этот стек, не может
// присутствовать в свежесозданном proc->page_table: kernel_cr3 был снят
// до того, как хоть один процесс успел выделить себе пользовательский
// стек, а sync_kernel_mappings() синхронизирует только kernel-space
// (256-511). Переключение CR3 мгновенно обрывало сам стек вызовов —
// instruction fetch следующей же инструкции ронял систему в triple fault.
// Вместо этого маппим напрямую в proc->page_table по физическому
// указателю через map_page_in_pml4() (identity mapping), как это уже
// делает elf_load_to_process() для сегментов ELF.
static uint64_t allocate_ring0_stack(process_t *proc) {
    uint64_t stack_base = KERNEL_STACK_AREA_START +
                          (proc->pid * KERNEL_STACK_SIZE);
    size_t num_pages = KERNEL_STACK_SIZE / PAGE_SIZE;

    // Хранит ФИЗИЧЕСКИЕ адреса выделенных страниц (используется
    // free_ring0_stack() — без переключения CR3 виртуальный адрес другого
    // процесса нельзя транслировать через текущую активную таблицу).
    uint64_t *pages = (uint64_t*)kmalloc(sizeof(uint64_t) * num_pages);
    if (!pages) return 0;

    for (size_t i = 0; i < num_pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            for (size_t j = 0; j < i; j++) pmm_free_page(pages[j]);
            kfree(pages);
            return 0;
        }

        uint64_t virt = stack_base + i * PAGE_SIZE;
        if (map_page_in_pml4(proc->page_table, virt, phys, PAGE_PRESENT | PAGE_WRITE) != 0) {
            pmm_free_page(phys);
            for (size_t j = 0; j < i; j++) pmm_free_page(pages[j]);
            kfree(pages);
            return 0;
        }
        pages[i] = phys;
    }

    proc->ring0_stack_pages = (uint64_t)pages;
    return stack_base + KERNEL_STACK_SIZE;
}

// Освобождение Ring 0 стека — тоже без переключения CR3, просто
// освобождает уже известные физические страницы (сама proc->page_table
// либо ещё не используется ни одним процессом, либо целиком отбрасывается
// вызывающим кодом — разбор PML4 целиком в этом файле нигде не делается,
// см. process_reap()).
static void free_ring0_stack(process_t *proc) {
    if (!proc->ring0_stack_pages) return;

    uint64_t *pages = (uint64_t*)proc->ring0_stack_pages;
    size_t num_pages = KERNEL_STACK_SIZE / PAGE_SIZE;

    for (size_t i = 0; i < num_pages; i++) {
        if (pages[i]) pmm_free_page(pages[i]);
    }

    kfree(pages);
    proc->ring0_stack_pages = 0;
    proc->ring0_stack = 0;
}

// Создаёт новое адресное пространство на основе КОРНЕВОГО ядерного PML4
static uint64_t create_address_space(uint64_t kernel_pml4_phys) {
    uint64_t new_pml4_phys = pmm_alloc_page();
    if (!new_pml4_phys) return 0;
    
    uint64_t *new_pml4 = (uint64_t*)new_pml4_phys;
    uint64_t *kernel_pml4 = (uint64_t*)kernel_pml4_phys;

    // pmm_alloc_page() НЕ гарантирует нулевую страницу — в ней остаётся
    // мусор от предыдущего владельца этой физической страницы (или от
    // прошивки, если страница вообще ни разу не использовалась). Цикл
    // ниже пишет new_pml4[i] только там, где соответствующая запись ЕСТЬ
    // в kernel_pml4 (например, kernel_cr3 захвачен ДО того, как хоть один
    // процесс выделил себе стек, так что записей для
    // KERNEL_STACK_AREA_START/USER_STACK_AREA_START там нет и не может
    // быть) — без обнуления в остальных индексах остался бы мусор, и если
    // в нём случайно выставлен бит PRESENT, он воспринимался бы как
    // указатель на настоящую PDPT.
    for (int i = 0; i < 512; i++) {
        new_pml4[i] = 0;
    }

    // Копируем ВСЕ записи, но с модификацией флагов
    for (int i = 0; i < 512; i++) {
        // Индекс 0 (identity map низких физических гигабайт, см.
        // paging_init()) НЕ копируем по указателю сюда — иначе все процессы
        // разделяли бы один и тот же физический PDPT/PD, и расщепление
        // huge-страницы под ELF одного процесса (см. clone_low_identity_map()
        // в paging.c) портило бы identity map для всей системы. Вместо
        // этого ниже даём процессу СОБСТВЕННУЮ копию.
        if (i == 0) continue;

        if (kernel_pml4[i] & PAGE_PRESENT) {
            uint64_t entry = kernel_pml4[i];

            // Для user space (0-255) - снимаем флаг USER
            if (i < 256) {
                entry &= ~PAGE_USER;  // Убираем доступ из user mode
            }

            new_pml4[i] = entry;
        }
    }

    clone_low_identity_map(new_pml4_phys);

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

// Тоже без переключения CR3 (см. подробное объяснение у
// allocate_ring0_stack()) — маппим прямо в pml4_phys через
// map_page_in_pml4().
static uint64_t allocate_user_stack(size_t size, uint64_t pml4_phys, uint32_t pid) {
    // Каждому процессу - свой уникальный виртуальный адрес
    uint64_t stack_virt = USER_STACK_AREA_START + (pid * USER_STACK_SIZE);
    size_t num_pages = size / PAGE_SIZE;

    if (num_pages == 0) num_pages = 1;

    irq_disable();

    for (size_t i = 0; i < num_pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            // Откат
            for (size_t j = 0; j < i; j++) {
                uint64_t v = stack_virt + j * PAGE_SIZE;
                uint64_t p = get_physical_address_in_pml4(pml4_phys, v);
                if (p) pmm_free_page(p);
            }
            irq_enable();
            return 0;
        }

        uint64_t virt = stack_virt + i * PAGE_SIZE;
        if (map_page_in_pml4(pml4_phys, virt, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER) != 0) {
            pmm_free_page(phys);
            for (size_t j = 0; j < i; j++) {
                uint64_t v = stack_virt + j * PAGE_SIZE;
                uint64_t p = get_physical_address_in_pml4(pml4_phys, v);
                if (p) pmm_free_page(p);
            }
            irq_enable();
            return 0;
        }
    }

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

    // Заполняем стек пользователя — БЕЗ переключения CR3 (см. подробное
    // объяснение у allocate_ring0_stack()): пишем напрямую по физическим
    // адресам через get_physical_address_in_pml4(), не трогая текущий
    // (вызывающий) стек вызовов.
    uint64_t rsp = proc->stack_base;

    rsp -= 8;
    uint64_t phys = get_physical_address_in_pml4(new_pml4, rsp);
    *(uint64_t*)phys = (uint64_t)process_exit;

    rsp -= 8;
    phys = get_physical_address_in_pml4(new_pml4, rsp);
    *(uint64_t*)phys = (uint64_t)entry;

    rsp -= 8;
    phys = get_physical_address_in_pml4(new_pml4, rsp);
    *(uint64_t*)phys = 0x202;

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

    // idle_process — это узел общего кольцевого списка (нужен там, чтобы
    // schedule() всегда имел кого выбрать), но после ПЕРВОГО же
    // переключения на любой другой процесс его state становится обычным
    // PROCESS_READY — и без исключения ниже он выбирался бы этим циклом
    // наравне с shell/любым hello.elf в порядке обычной круговой очереди.
    // Тогда keyboard/timer IRQ, который прерывает "текущий" процесс в
    // произвольный момент, иногда попадал прямо на idle — и вся команда
    // шелла (run/kill/exec), выполняющаяся синхронно внутри этого IRQ,
    // работала так, будто current_process == idle (PID 0): process_create()
    // проставлял ppid=0 вместо реального родителя (ломая kill+wait/reap), а
    // exec ЗАМЕНЯЛ САМ IDLE — у которого нет ring0_stack (0) — так что
    // первый же syscall новой программы уводил ядерный стек (current_kernel_rsp)
    // на 0 и ронял систему. idle должен выбираться ТОЛЬКО запасной веткой
    // ниже, когда реально больше некого выбрать — никогда этим циклом.
    while (next &&
           (next->state != PROCESS_READY || next == idle_process) &&
           tries < MAX_TRIES)
    {
        next = next->next;
        tries++;

        if (next == current_process)
            break;
    }

    if (!next ||
        next->state != PROCESS_READY ||
        next == idle_process ||
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

        case PROCESS_STOPPED:
            return "STOPPED";

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

// Общая часть SIGKILL/SIGTERM (единственная разница между ними в этом
// ядре — только в exit_code, т.к. пользовательских обработчиков сигналов
// нет и оба в итоге просто завершают процесс).
static int terminate_process_by_signal(process_t *p, int sig) {
    if (p->state == PROCESS_TERMINATED)
        return 0;

    printf("[PROCESS] PID %u ('%s') terminated by signal %d\n",
           p->pid, p->name, sig);

    // Как и в настоящих шеллах: $? для процесса, убитого сигналом, — 128+sig.
    p->exit_code = 128 + sig;
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

int process_signal(uint32_t pid, int sig)
{
    if (!process_list)
        return -1;

    process_t *p = process_list;

    do {
        if (p->pid == pid) {

            if (p == idle_process)
                return -1;

            switch (sig) {
                case SIGKILL:
                case SIGTERM:
                    return terminate_process_by_signal(p, sig);

                case SIGSTOP:
                    if (p->state == PROCESS_TERMINATED)
                        return 0;

                    printf("[PROCESS] PID %u ('%s') stopped\n",
                           p->pid, p->name);
                    p->state = PROCESS_STOPPED;

                    if (p == current_process) {
                        // Останавливаем сами себя: уступаем CPU и
                        // возобновимся здесь же, когда придёт SIGCONT.
                        schedule();
                    }
                    return 0;

                case SIGCONT:
                    if (p->state == PROCESS_STOPPED) {
                        printf("[PROCESS] PID %u ('%s') continued\n",
                               p->pid, p->name);
                        p->state = PROCESS_READY;
                    }
                    return 0;

                default:
                    return -1;
            }
        }

        p = p->next;

    } while (p && p != process_list);

    return -1;
}

int process_kill(uint32_t pid)
{
    return process_signal(pid, SIGKILL);
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
    uint64_t align_pad; // соответствует "pushq $0" в syscall_entry.S
    uint64_t user_rsp;  // user RSP на момент ЭТОГО конкретного syscall —
                        // сохранён на СОБСТВЕННОМ ядерном стеке процесса
                        // (а не в общей на всё ядро переменной), поэтому
                        // остаётся верным независимо от того, сколько
                        // других процессов успеют сделать свои syscall'ы,
                        // пока этот вызов приостановлен в schedule().
} syscall_frame_t;

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
    // быть у настоящего fork(). vfs_dup_fd() увеличивает inode->ref_count
    // и (для пайпов) readers/writers — этим счётчикам иначе неоткуда
    // узнать, что у одного и того же file_t теперь два независимых
    // "владельца" (родитель и ребёнок), каждый со своим будущим
    // vfs_close().
    child->fd_table = parent->fd_table;
    for (int i = 0; i < MAX_FD_PER_PROCESS; i++) {
        file_t *f = child->fd_table.files[i];
        if (f) {
            vfs_dup_fd(f);
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
    ctx->rsp = frame->user_rsp;
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