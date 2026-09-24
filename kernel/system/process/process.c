#include "process.h"
#include "system/timer/pit.h"
#include "system/mm/heap.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "system/cpu/gdt.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"
#include "lib/string.h"
#include "system/devmode/devmode.h"
#include "system/klog/klog.h"
#include "fs/lufirafs/lufirafs.h"

#ifndef PAGE_PS
#define PAGE_PS 0x80    // Page size (2MB/1GB) — как и в elf.c
#endif

process_t *process_list = NULL;
process_t *current_process = NULL;
volatile uint32_t foreground_pid = 0;
volatile int shell_is_respawn = 0;
static void (*shell_entry_fn)(void) = NULL;

void process_set_shell_entry(void (*entry)(void)) {
    shell_entry_fn = entry;
}

// Пересоздаёт процесс "shell", если завершающийся процесс был is_shell —
// см. подробный комментарий у поля is_shell в process.h. Вызывается из
// process_exit() и terminate_process_by_signal() ДО возможного
// switch_to_process()/schedule() ниже, чтобы новый шелл сразу же оказался
// валидным READY-кандидатом для этого же самого переключения.
static void respawn_shell_if_needed(process_t *dying) {
    if (!dying->is_shell || !shell_entry_fn)
        return;

    process_t *respawned = process_create("shell", shell_entry_fn);
    if (respawned) {
        respawned->is_shell = 1;
        shell_is_respawn = 1;
    }
}
uint64_t current_kernel_rsp = 0; // Глобальная переменная для asm
static uint32_t next_pid = 1;
static process_t *idle_process = NULL;
uint64_t kernel_cr3 = 0;

// Живых process_t, созданных через process_create() (idle не в счёте — он
// kmalloc()'ится напрямую в process_init() и никогда не уничтожается).
// MAX_PROCESSES был объявлен (process.h), но нигде не проверялся:
// process_create() создавал процессы без ограничения, пока не кончится
// физическая память. Инкремент — на успешном пути process_create(),
// декремент — в единственной точке, через которую проходит любое реальное
// уничтожение process_t (process_remove_from_list(), см. ниже).
static uint32_t process_count = 0;

// Счётчик вложенных запретов прерываний. irq_disable()/irq_enable() иногда
// вызываются уже ИЗНУТРИ обработчика IRQ (например, шелл выполняет команды
// прямо в keyboard_irq_handler()), где прерывания уже отключены самим CPU —
// поэтому запоминаем реальное состояние EFLAGS.IF на первом захвате и делаем
// sti обратно, только если они действительно были включены.
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

// Выделение Ring 0 стека. НЕ переключает CR3: process_create() обычно
// вызывается прямо из keyboard_irq_handler(), то есть текущий стек вызовов
// лежит на user-стеке вызывающего процесса, чей PML4-индекс ещё не
// синхронизирован в свежесозданный proc->page_table — переключение CR3 тут
// оборвало бы сам стек вызовов. Маппим напрямую по физическому адресу через
// map_page_in_pml4(), как elf_load_to_process() для сегментов ELF.
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

// Закрывает все fd процесса (через vfs_close(), чтобы пайпы корректно
// осиротевали) и освобождает его адресное пространство + ring0-стек. Единая
// точка перед КАЖДЫМ реальным уничтожением process_t. vfs_close() работает
// через current_fd_table, а не current_process, поэтому временная подмена
// этого глобального указателя безопасна и для чужого (не вызывающего)
// процесса.
static void free_process_resources(process_t *proc) {
    fd_table_t *saved = current_fd_table;
    current_fd_table = &proc->fd_table;
    for (int i = 0; i < MAX_FD_PER_PROCESS; i++) {
        if (current_fd_table->files[i]) vfs_close(i);
    }
    current_fd_table = saved;

    if (proc->page_table) free_user_address_space(proc->page_table);
    free_ring0_stack(proc);
}

// Создаёт новое адресное пространство на основе КОРНЕВОГО ядерного PML4
static uint64_t create_address_space(uint64_t kernel_pml4_phys) {
    uint64_t new_pml4_phys = pmm_alloc_page();
    if (!new_pml4_phys) return 0;
    
    uint64_t *new_pml4 = (uint64_t*)phys_to_virt(new_pml4_phys);
    uint64_t *kernel_pml4 = (uint64_t*)phys_to_virt(kernel_pml4_phys);

    // pmm_alloc_page() не гарантирует нулевую страницу — обнуляем, иначе
    // мусор со случайно выставленным PRESENT сойдёт за указатель на PDPT.
    for (int i = 0; i < 512; i++) {
        new_pml4[i] = 0;
    }

    for (int i = 0; i < 512; i++) {
        // Индекс 0 (identity map, см. paging_init()) не копируем по указателю —
        // иначе все процессы делили бы один физический PDPT/PD, и расщепление
        // huge-страницы под ELF одного процесса портило бы identity map всей
        // системы. Вместо этого ниже даём процессу собственную копию.
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

// Глубокий клон адресного пространства для fork(): ядерная половина
// (256-511) заводится штатно (create_address_space + sync_kernel_mappings),
// пользовательская (0-255) рекурсивно обходится и копируется eager (без
// copy-on-write — инфраструктуры под COW в paging.c пока нет). При нехватке
// памяти возвращает 0; частично выделенное дерево не освобождается (как и
// везде в этом файле — полного разбора PML4 пока нет, см. process_reap()).
static uint64_t clone_address_space_deep(uint64_t src_pml4_phys) {
    uint64_t new_pml4_phys = create_address_space(kernel_cr3);
    if (!new_pml4_phys)
        return 0;

    uint64_t current_kernel_pml4;
    asm volatile("mov %%cr3, %0" : "=r"(current_kernel_pml4));
    sync_kernel_mappings(new_pml4_phys, current_kernel_pml4);

    uint64_t *src_pml4 = (uint64_t*)phys_to_virt(src_pml4_phys);
    uint64_t *dst_pml4 = (uint64_t*)phys_to_virt(new_pml4_phys);

    for (int pml4_idx = 0; pml4_idx < 256; pml4_idx++) {
        if (!(src_pml4[pml4_idx] & PAGE_PRESENT))
            continue;

        uint64_t new_pdpt_phys = pmm_alloc_page();
        if (!new_pdpt_phys) return 0;
        uint64_t *new_pdpt = (uint64_t*)phys_to_virt(new_pdpt_phys);
        for (int i = 0; i < 512; i++) new_pdpt[i] = 0;

        uint64_t *src_pdpt = (uint64_t*)phys_to_virt(src_pml4[pml4_idx] & 0x000FFFFFFFFFF000ULL);

        for (int pdpt_idx = 0; pdpt_idx < 512; pdpt_idx++) {
            if (!(src_pdpt[pdpt_idx] & PAGE_PRESENT))
                continue;

            uint64_t new_pd_phys = pmm_alloc_page();
            if (!new_pd_phys) return 0;
            uint64_t *new_pd = (uint64_t*)phys_to_virt(new_pd_phys);
            for (int i = 0; i < 512; i++) new_pd[i] = 0;

            uint64_t *src_pd = (uint64_t*)phys_to_virt(src_pdpt[pdpt_idx] & 0x000FFFFFFFFFF000ULL);

            for (int pd_idx = 0; pd_idx < 512; pd_idx++) {
                if (!(src_pd[pd_idx] & PAGE_PRESENT))
                    continue;

                if (src_pd[pd_idx] & PAGE_PS) {
                    // Пользовательские маппинги никогда не huge, но это же
                    // место в PML4[0] занимает разделяемый identity-map ядра
                    // (huge 2MB-страницы, build_identity_pdpt()). Такую
                    // запись нельзя пропускать — иначе у ребёнка после
                    // fork() дыра ровно там, где должен быть код самого
                    // ядра, и первая же его инструкция после переключения CR3
                    // валит систему в page fault. Копируем запись по
                    // значению — физическая память ядра общая для всех
                    // процессов и не копируется по-настоящему.
                    new_pd[pd_idx] = src_pd[pd_idx];
                    continue;
                }

                uint64_t new_pt_phys = pmm_alloc_page();
                if (!new_pt_phys) return 0;
                uint64_t *new_pt = (uint64_t*)phys_to_virt(new_pt_phys);
                for (int i = 0; i < 512; i++) new_pt[i] = 0;

                uint64_t *src_pt = (uint64_t*)phys_to_virt(src_pd[pd_idx] & 0x000FFFFFFFFFF000ULL);

                for (int pt_idx = 0; pt_idx < 512; pt_idx++) {
                    if (!(src_pt[pt_idx] & PAGE_PRESENT))
                        continue;

                    uint64_t src_phys = src_pt[pt_idx] & 0x000FFFFFFFFFF000ULL;
                    uint64_t flags = src_pt[pt_idx] & (0xFFFULL | PAGE_NX);

                    uint64_t new_phys = pmm_alloc_page();
                    if (!new_phys) return 0;

                    // Через kernel physmap (phys_to_virt) — НЕ через
                    // "низкую" identity-карту (virt==phys): src_phys здесь
                    // может оказаться физической страницей, которая у
                    // САМОГО форкающегося процесса (fork() выполняется под
                    // ЕГО собственным CR3) занята под что-то другое в его
                    // же ELF-хайджекнутом диапазоне 0x400000+ — см.
                    // подробное объяснение у phys_to_virt() в paging.h.
                    memcpy(phys_to_virt(new_phys), phys_to_virt(src_phys), PAGE_SIZE);

                    new_pt[pt_idx] = (new_phys & 0x000FFFFFFFFFF000ULL) | flags;
                }

                new_pd[pd_idx] = (new_pt_phys & 0x000FFFFFFFFFF000ULL) |
                                 (src_pd[pd_idx] & 0xFFFULL);
            }

            new_pdpt[pdpt_idx] = (new_pd_phys & 0x000FFFFFFFFFF000ULL) |
                                 (src_pdpt[pdpt_idx] & 0xFFFULL);
        }

        dst_pml4[pml4_idx] = (new_pdpt_phys & 0x000FFFFFFFFFF000ULL) |
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
    idle_process->is_shell = 0;
    // idle не создаётся через process_create() (kmalloc'ится напрямую), так
    // что это поле надо выставить явно: idle живёт целиком в ring0 в своём
    // hlt-цикле и не должен НИКОГДА идти через context_enter_ring3() (у него
    // ring0_stack=0 — там просто некуда строить iretq-кадр для ring3).
    idle_process->first_run = 0;
    // idle никогда не вызывает mmap, но kmalloc() тут ничего не зануляет
    // (см. heap.c) — оставить как есть значило бы читать мусор, если это
    // поле вообще когда-нибудь тронут.
    idle_process->next_mmap_addr = MMAP_AREA_START;
    memset(idle_process->mmap_regions, 0, sizeof(idle_process->mmap_regions));
    idle_process->cwd_inode = LUFIRAFS_ROOT_INODE;
    idle_process->cwd_path[0] = '/';
    idle_process->cwd_path[1] = '\0';
    // idle — точка отсчёта identity для всей системы (первый shell
    // наследует от него через process_create()), так что он обязан быть
    // root, а не мусором из kmalloc().
    idle_process->uid = 0;
    idle_process->gid = 0;

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

    DLOG("[PROCESS] Process manager initialized\n");
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

// Пишет len байт из src (kernel-side) по виртуальному адресу dst_virt В
// АДРЕСНОМ ПРОСТРАНСТВЕ pml4_phys, БЕЗ переключения CR3 — постранично,
// через get_physical_address_in_pml4()+phys_to_virt(), тот же приём, что
// уже используется в elf_load_to_process() (elf.c) для копирования
// сегментов ELF. dst_virt не обязан быть выровнен на границу страницы (и
// в build_exec_stack() ниже почти никогда не бывает).
static void write_user_bytes(uint64_t pml4_phys, uint64_t dst_virt,
                              const void *src, uint64_t len) {
    const uint8_t *s = (const uint8_t*)src;
    uint64_t remaining = len;
    while (remaining > 0) {
        uint64_t page_off = dst_virt & (PAGE_SIZE - 1);
        uint64_t chunk = PAGE_SIZE - page_off;
        if (chunk > remaining) chunk = remaining;

        // get_physical_address_in_pml4() уже возвращает ТОЧНЫЙ (не
        // выровненный по странице) физический адрес байта dst_virt — сам
        // page_off внутри него уже учтён (см. её реализацию в paging.c:
        // "+ (virt & 0xFFF)"). Прибавлять page_off ЕЩЁ РАЗ здесь — как это
        // корректно делает elf_load_to_process() в elf.c, но там phys
        // берётся от ВЫРОВНЕННОГО page_va, а не от точного dst_virt —
        // было бы двойным учётом смещения и уводило бы запись мимо цели.
        uint64_t phys = get_physical_address_in_pml4(pml4_phys, dst_virt);
        if (phys) memcpy(phys_to_virt(phys), s, chunk);

        dst_virt += chunk;
        s += chunk;
        remaining -= chunk;
    }
}

static void write_user_u64(uint64_t pml4_phys, uint64_t dst_virt, uint64_t val) {
    write_user_bytes(pml4_phys, dst_virt, &val, sizeof(val));
}

int build_exec_stack(uint64_t new_pml4, uint64_t stack_top,
                      char *const argv[], char *const envp[],
                      uint64_t *rsp_out, uint64_t *argv_out, uint64_t *envp_out)
{
    int argc = 0, envc = 0;
    if (argv) while (argc < MAX_EXEC_ARGS && argv[argc]) argc++;
    if (envp) while (envc < MAX_EXEC_ARGS && envp[envc]) envc++;
    if ((argv && argv[argc]) || (envp && envp[envc]))
        return -1;   // не нашли NULL-терминатор в разумных пределах

    uint64_t total = (uint64_t)(argc + 1) * 8 + (uint64_t)(envc + 1) * 8;
    for (int i = 0; i < argc; i++) total += strlen(argv[i]) + 1;
    for (int i = 0; i < envc; i++) total += strlen(envp[i]) + 1;
    if (total > MAX_EXEC_ARGS_BYTES)
        return -1;

    uint64_t argv_ptrs[MAX_EXEC_ARGS];
    uint64_t envp_ptrs[MAX_EXEC_ARGS];
    uint64_t cursor = stack_top;

    // Сами строки — куда угодно ниже stack_top, порядок не важен (только
    // порядок ЗАПИСИ указателей в массивы ниже сохраняет argv[i]/envp[i]).
    for (int i = 0; i < argc; i++) {
        uint64_t len = strlen(argv[i]) + 1;
        cursor -= len;
        write_user_bytes(new_pml4, cursor, argv[i], len);
        argv_ptrs[i] = cursor;
    }
    for (int i = 0; i < envc; i++) {
        uint64_t len = strlen(envp[i]) + 1;
        cursor -= len;
        write_user_bytes(new_pml4, cursor, envp[i], len);
        envp_ptrs[i] = cursor;
    }

    // envp[] — NULL-терминированный массив указателей.
    cursor -= 8;
    write_user_u64(new_pml4, cursor, 0);
    for (int i = envc - 1; i >= 0; i--) {
        cursor -= 8;
        write_user_u64(new_pml4, cursor, envp_ptrs[i]);
    }
    uint64_t envp_addr = cursor;

    // argv[] — то же самое.
    cursor -= 8;
    write_user_u64(new_pml4, cursor, 0);
    for (int i = argc - 1; i >= 0; i--) {
        cursor -= 8;
        write_user_u64(new_pml4, cursor, argv_ptrs[i]);
    }
    uint64_t argv_addr = cursor;

    *rsp_out = cursor;
    *argv_out = argv_addr;
    *envp_out = envp_addr;
    return 0;
}

process_t* process_create(const char *name, void (*entry)(void)) {
    irq_disable();

    if (process_count >= MAX_PROCESSES) {
        irq_enable();
        return NULL;
    }

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
    proc->is_shell = 0;
    // first_run=1 (→ настоящий вход в ring3 через context_enter_ring3())
    // только когда entry==NULL, т.е. вызывающий сам допишет ctx.rip на
    // пользовательский ELF-адрес позже. Когда entry передан напрямую (ядерная
    // функция вроде shell_task) процесс должен остаться в ring0 через обычный
    // context_switch() — иначе ring3 попытается исполнить код ядра без
    // PAGE_USER и упадёт в page fault.
    proc->first_run = (entry == NULL);
    proc->next_mmap_addr = MMAP_AREA_START;
    memset(proc->mmap_regions, 0, sizeof(proc->mmap_regions));
    proc->cwd_inode = LUFIRAFS_ROOT_INODE;
    proc->cwd_path[0] = '/';
    proc->cwd_path[1] = '\0';
    // В отличие от cwd (всегда сбрасывается на корень), identity
    // НАСЛЕДУЕТСЯ от текущего процесса — иначе run/runbg/exec стартовали бы
    // новый процесс всегда как root независимо от того, кто его запустил.
    proc->uid = current_process ? current_process->uid : 0;
    proc->gid = current_process ? current_process->gid : 0;

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
        // free_user_address_space(), не голый pmm_free_page(): create_address_space()
        // уже установила в new_pml4 приватную copy identity-map через
        // clone_low_identity_map() — bare pmm_free_page() освобождал бы
        // только саму страницу PML4, оставляя эту копию висеть навсегда.
        free_user_address_space(new_pml4);
        kfree(proc);
        irq_enable();
        return NULL;
    }

    // Выделяем пользовательский стек
    proc->stack_size = 16384;
    proc->stack_base = allocate_user_stack(proc->stack_size, new_pml4, proc->pid);
    if (!proc->stack_base) {
        free_ring0_stack(proc);
        free_user_address_space(new_pml4);
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
    *(uint64_t*)phys_to_virt(phys) = (uint64_t)process_exit;

    rsp -= 8;
    phys = get_physical_address_in_pml4(new_pml4, rsp);
    *(uint64_t*)phys_to_virt(phys) = (uint64_t)entry;

    rsp -= 8;
    phys = get_physical_address_in_pml4(new_pml4, rsp);
    *(uint64_t*)phys_to_virt(phys) = 0x202;

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
    
    DLOG("[PROCESS] Created '%s' (PID %u, Ring 3, user stack: 0x%lx)\n",
           name, proc->pid, proc->stack_base);
    klog("[PROCESS] created '%s' (PID %u)", name, proc->pid);

    process_count++;

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
// кольцевого списка процессов, освобождает его ресурсы
// (free_process_resources() — page_table/ring0-стек/открытые fd) и сам
// process_t.
static void process_remove_from_list(process_t *target) {
    if (!process_list || !target)
        return;

    // Как и в process_wait() (единственный внешний вызывающий, чей скан уже
    // защищён своим собственным irq_disable() — но process_discard() зовёт
    // это напрямую, без такой защиты) — таймерный тик может прилететь и
    // запустить process_reap(), который тоже правит process_list, прямо
    // посреди этой правки.
    irq_disable();

    if (target->next == target) {
        if (process_list == target)
            process_list = NULL;
        free_process_resources(target);
        process_count--;
        kfree(target);
        irq_enable();
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

    free_process_resources(target);
    process_count--;
    kfree(target);
    irq_enable();
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

    DLOG("\n[PROCESS] Process %u ('%s') exiting (code %d)\n",
           exiting_process->pid,
           exiting_process->name,
           exit_code);
    klog("[PROCESS] PID %u ('%s') exited (code %d)",
         exiting_process->pid, exiting_process->name, exit_code);

    exiting_process->exit_code = exit_code;
    exiting_process->state = PROCESS_TERMINATED;

    if (exiting_process->pid == foreground_pid)
        foreground_pid = 0;

    respawn_shell_if_needed(exiting_process);

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

        // irq_disable()/irq_enable() вокруг всего скана+удаления: process_reap()
        // вызывается прямо из timer_irq_handler() на каждом тике, так что
        // прерывание могло бы прилететь посреди обхода process_list и
        // удалить/освободить другой узел, пока мы держим на него p/p->next.
        irq_disable();

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

            irq_enable();

            if (status_out)
                *status_out = code;

            return (int)zpid;
        }

        if (!has_child) {
            // У вызывающего нет (или больше нет) такого ребёнка.
            irq_enable();
            return -1;
        }

        irq_enable();

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
        // Не голый pmm_free_page() — см. тот же комментарий в process_create():
        // create_address_space() уже установила приватную copy identity-map.
        free_user_address_space(new_pml4);
        return -1;
    }

    *new_pml4_out = new_pml4;
    *new_stack_out = new_stack;
    return 0;
}

// Подтверждает exec(): переключает proc на уже подготовленные адресное
// пространство и стек (замена образа процесса на месте, PID не меняется).
// Старые page_table/стек не освобождаются — разбора PML4 в кодовой базе
// пока нет вообще (process_reap() тоже его не делает).
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

    // exec() заменяет ВСЁ адресное пространство новым new_pml4 — старые
    // mmap-регионы (как и старый стек/образ) физически больше не
    // существуют в нём. Без сброса здесь процесс унаследовал бы учёт,
    // указывающий на память, которой в его свежем адресном пространстве
    // просто нет.
    proc->next_mmap_addr = MMAP_AREA_START;
    memset(proc->mmap_regions, 0, sizeof(proc->mmap_regions));

    // cwd (proc->cwd_path/cwd_inode) НЕ сбрасывается здесь — в отличие от
    // mmap-регионов, exec() не должен менять текущий каталог процесса
    // (POSIX execve() тоже сохраняет cwd).

    // uid/gid тоже сознательно НЕ трогаются — POSIX execve() сохраняет
    // identity процесса, кроме случая setuid-бита на исполняемом файле,
    // которого в этой минимальной реализации нет вообще (см. process.h).

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
            DLOG("[PROCESS] Reaping zombie PID %u ('%s')\n", p->pid, p->name);
            
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

                free_process_resources(p);
                process_count--;
                kfree(p);
                p = process_list;
                prev = NULL;
                continue;
            } else {
                prev->next = p->next;
                free_process_resources(p);
                process_count--;
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

    // idle_process живёт в общем кольцевом списке как обычный PROCESS_READY
    // узел, но не должен выбираться этим циклом наравне с реальными
    // процессами (exec подменял бы сам idle, у которого нет ring0_stack —
    // первый же syscall уронил бы систему). idle — только запасной вариант
    // ниже, когда реально больше некого выбрать.
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
        // Больше некого выбрать. Если current_process ещё RUNNING, он просто
        // "на всякий случай" уступил — можно продолжить исполняться им же, не
        // переключая на idle (раньше это было безусловно idle, из-за чего
        // current_process/idle пинг-понговали каждый тик и IRQ-код видел
        // idle вместо реального current_process). Если же current_process
        // САМ только что перевёл себя в SLEEPING/BLOCKED/TERMINATED —
        // продолжать им нельзя (caller вроде process_sleep() как ни в чём не
        // бывало продолжил бы сразу после return), и единственный кандидат —
        // idle.
        if (current_process->state == PROCESS_RUNNING) {
            irq_enable();
            return;
        }

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

    // Самая первая активация процесса идёт через настоящий ring0->ring3
    // переход (iretq) — см. process_t.first_run в process.h — а не через
    // обычный context_switch() (jmp, CS/CPL не меняются). Все последующие
    // резюме (включая процесс, вытесненный планировщиком прямо из ring3)
    // всегда используют context_switch(): к тому моменту это уже не
    // "холодный старт", а возобновление прерванного вызова.
    if (next->first_run) {
        next->first_run = 0;
        context_enter_ring3(prev_context, &next->context);
    } else {
        context_switch(prev_context, &next->context);
    }
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
    klog("[PROCESS] PID %u ('%s') terminated by signal %d", p->pid, p->name, sig);

    // Как и в настоящих шеллах: $? для процесса, убитого сигналом, — 128+sig.
    p->exit_code = 128 + sig;
    p->state = PROCESS_TERMINATED;

    // Это нужно сделать ДО возможного switch_to_process()/schedule() ниже
    // (когда p — сам current_process, этот вызов может никогда не
    // вернуться сюда обычным путём) — иначе Ctrl+C, убивший процесс, пока
    // тот был "текущим", оставил бы foreground_pid висеть на уже мёртвом
    // PID навсегда.
    if (p->pid == foreground_pid)
        foreground_pid = 0;

    // Тем же поводом (может не вернуться сюда обычным путём, если p ==
    // current_process): если убитый процесс был is_shell (exec когда-то
    // "надел" его образ поверх шелла), система осталась бы вообще без
    // интерактивного приглашения — пересоздаём шелл ДО переключения.
    respawn_shell_if_needed(p);

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
                case SIGINT:
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
    // Не голый pmm_free_page(): process_create() уже полностью построил
    // этот плейсхолдер (собственная copy identity-map через
    // clone_low_identity_map() + выделенный allocate_user_stack()'ом
    // пользовательский стек) — bare pmm_free_page() освобождал бы только
    // саму страницу PML4, оставляя всё остальное висеть навсегда при
    // каждом fork().
    free_user_address_space(placeholder_pml4);

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

    // clone_address_space_deep() выше уже физически продублировала страницы
    // под всеми mmap-регионами родителя (это просто present-записи в
    // диапазоне PML4[0..255], как и всё остальное адресное пространство) —
    // тут нужно только скопировать сам учёт, иначе ребёнок "не будет знать"
    // об унаследованных регионах (не сможет их munmap()) и начнёт свой
    // bump-указатель заново с MMAP_AREA_START, затирая своими будущими mmap()
    // то, что уже унаследовал от родителя по тем же адресам.
    child->next_mmap_addr = parent->next_mmap_addr;
    memcpy(child->mmap_regions, parent->mmap_regions, sizeof(parent->mmap_regions));

    // POSIX: ребёнок наследует cwd родителя 1:1.
    child->cwd_inode = parent->cwd_inode;
    strcpy(child->cwd_path, parent->cwd_path);

    // POSIX: ребёнок наследует identity родителя 1:1 (нет setuid-бита в
    // этой реализации, так что тут просто копия, без exec-time пересчёта).
    child->uid = parent->uid;
    child->gid = parent->gid;

    // Контекст ребёнка продолжает выполнение сразу после syscall в
    // родителе (тот же rip/rflags/callee-saved регистры), но с rax=0 — это
    // и есть возвращаемое значение fork() для ребёнка. В отличие от
    // родителя (штатный iretq в ring3), ребёнок стартует через обычный
    // context_switch()/jmp и первое время выполняется в ring0, как любой
    // новый процесс, до своего первого системного вызова.
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

    DLOG("[FORK] PID %u forked into PID %u\n", parent->pid, child->pid);
    klog("[FORK] PID %u forked into PID %u", parent->pid, child->pid);

    return (uint64_t)child->pid;
}