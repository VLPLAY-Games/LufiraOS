#include "process.h"
#include "system/timer/pit.h"
#include "system/mm/heap.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "system/mm/shm.h"
#include "system/cpu/gdt.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"
#include "lib/string.h"
#include "system/devmode/devmode.h"
#include "system/klog/klog.h"
#include "fs/lufirafs/lufirafs.h"
#include "system/ipc/wm_protocol.h"

#ifndef PAGE_PS
#define PAGE_PS 0x80    // Page size (2MB/1GB) — как и в elf.c
#endif

process_t *process_list = NULL;
process_t *current_process = NULL;
volatile uint32_t foreground_pid = 0;
// v0.8 (GUI+WM), этап 3 — см. комментарий у объявления в process.h.
volatile uint32_t g_wm_pid = 0;
volatile int shell_is_respawn = 0;
static process_t *(*shell_spawner_fn)(void) = NULL;

void process_set_shell_spawner(process_t *(*spawner)(void)) {
    shell_spawner_fn = spawner;
}

// Пересоздаёт процесс "shell", если завершающийся процесс был is_shell (см.
// process.h). Вызывается ДО switch_to_process()/schedule() ниже, чтобы новый
// шелл сразу стал READY-кандидатом для этого же переключения.
// shell_is_respawn взводится здесь; spawner_fn (kernel.c, spawn_shell_process())
// читает его, чтобы решить, печатать баннер, и сам сбрасывает обратно в 0.
static void respawn_shell_if_needed(process_t *dying) {
    if (!dying->is_shell || !shell_spawner_fn)
        return;

    shell_is_respawn = 1;
    process_t *respawned = shell_spawner_fn();
    if (!respawned) {
        shell_is_respawn = 0;
        printf("[PROCESS] FATAL: failed to respawn shell\n");
    }
}
uint64_t current_kernel_rsp = 0; // Глобальная переменная для asm
static uint32_t next_pid = 1;
static process_t *idle_process = NULL;
uint64_t kernel_cr3 = 0;

// Живых process_t из process_create() (idle не в счёте — создаётся напрямую
// в process_init(), никогда не уничтожается). Инкремент на успешном пути
// process_create(), декремент — только в process_remove_from_list().
static uint32_t process_count = 0;

// Счётчик вложенных запретов прерываний. irq_disable()/irq_enable() иногда
// вызываются уже изнутри обработчика IRQ (прерывания уже отключены CPU),
// поэтому запоминаем EFLAGS.IF на первом захвате и делаем sti обратно только
// если они были включены.
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

// Выделение Ring 0 стека. НЕ переключает CR3: вызывающий стек ещё лежит на
// user-стеке текущего процесса, чей PML4 не синхронизирован с новым
// proc->page_table — переключение CR3 оборвало бы его. Маппим напрямую через
// map_page_in_pml4(), как elf_load_to_process() для сегментов ELF.
static uint64_t allocate_ring0_stack(process_t *proc) {
    uint64_t stack_base = KERNEL_STACK_AREA_START +
                          (proc->pid * KERNEL_STACK_SIZE);
    size_t num_pages = KERNEL_STACK_SIZE / PAGE_SIZE;

    // Храним физические адреса (для free_ring0_stack()) — без переключения
    // CR3 виртуальный адрес другого процесса не транслируется.
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

// Освобождение Ring 0 стека — без переключения CR3, просто освобождает уже
// известные физические страницы (полного разбора PML4 в этом файле нет,
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
// осиротевали) и освобождает адресное пространство + ring0-стек. Единая
// точка перед каждым реальным уничтожением process_t. vfs_close() работает
// через current_fd_table, так что временная подмена этого указателя безопасна
// и для чужого процесса.
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
        // системы. Ниже даём процессу собственную копию.
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
// пользовательская (0-255) копируется eager, рекурсивно (нет COW в
// paging.c). При нехватке памяти возвращает 0; частично выделенное дерево
// не освобождается (полного разбора PML4 нет, см. process_reap()).
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
                    // Пользовательские маппинги никогда не huge — это PML4[0],
                    // разделяемый identity-map ядра (huge 2MB, build_identity_pdpt()).
                    // Пропустить нельзя: у ребёнка была бы дыра там, где должен
                    // быть код ядра -> page fault сразу после переключения CR3.
                    // Копируем запись по значению, физическая память общая.
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

                    // MAP_SHARED-страница (shm.h): ребёнок должен видеть те же
                    // данные, что и родитель, в обе стороны — алиасим физкадр
                    // напрямую (без memcpy) и добавляем ссылку в shm.c.
                    if (src_pt[pt_idx] & PAGE_MMAP_SHARED) {
                        int id = shm_find_region_by_page(src_phys);
                        if (id >= 0) {
                            shm_add_ref(id);
                            new_pt[pt_idx] = src_pt[pt_idx]; // тот же физ. адрес и флаги как есть
                            continue;
                        }
                        // Область не нашлась (не должно случаться) — падаем на
                        // обычное копирование ниже: лучше лишняя приватная
                        // копия, чем алиас в никуда.
                    }

                    // PAGE_IDENTITY_SHARED снимается явно: memcpy() ниже
                    // кладёт содержимое в СОБСТВЕННУЮ страницу ребёнка — для
                    // него это уже личная память, которую его же
                    // free_user_address_space() обязан освободить (иначе
                    // страницы терялись бы для pmm на каждый fork(), см.
                    // PAGE_IDENTITY_SHARED в paging.h).
                    uint64_t flags = src_pt[pt_idx] & (0xFFFULL | PAGE_NX) & ~(uint64_t)PAGE_IDENTITY_SHARED;

                    uint64_t new_phys = pmm_alloc_page();
                    if (!new_phys) return 0;

                    // Через kernel physmap (phys_to_virt), не через identity:
                    // src_phys может быть страницей, занятой у форкающегося
                    // процесса чем-то другим в его ELF-диапазоне 0x400000+
                    // (fork() идёт под его собственным CR3) — см. phys_to_virt()
                    // в paging.h.
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
    // что первым входом в ring3 быть не должен (ring0_stack=0, iretq-кадр
    // некуда строить).
    idle_process->first_run = 0;
    // kmalloc() не зануляет (heap.c) — явная инициализация, хотя idle
    // mmap не вызывает.
    idle_process->next_mmap_addr = MMAP_AREA_START;
    memset(idle_process->mmap_regions, 0, sizeof(idle_process->mmap_regions));
    memset(idle_process->signal_handlers, 0, sizeof(idle_process->signal_handlers));
    idle_process->pending_signal = 0;
    idle_process->in_signal_handler = 0;
    idle_process->alarm_tick = 0;
    idle_process->sig_stack_top = 0;
    idle_process->cwd_inode = LUFIRAFS_ROOT_INODE;
    idle_process->cwd_path[0] = '/';
    idle_process->cwd_path[1] = '\0';
    // idle — точка отсчёта identity (первый shell наследует от него через
    // process_create()), поэтому обязан быть root, а не мусором из kmalloc().
    idle_process->uid = 0;
    idle_process->gid = 0;
    idle_process->cpu_ticks = 0;

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

int process_is_idle(void) {
    return current_process == idle_process;
}

// Без переключения CR3 (см. allocate_ring0_stack()) — маппим прямо в
// pml4_phys через map_page_in_pml4(). base_area/stride параметризуют
// "уникальный виртуальный адрес на pid", чтобы не дублировать функцию для
// SIGNAL_STACK_AREA_START (см. ниже) отдельно от USER_STACK_AREA_START.
static uint64_t allocate_user_stack_in(size_t size, uint64_t pml4_phys, uint32_t pid,
                                        uint64_t base_area, uint64_t stride) {
    // Каждому процессу - свой уникальный виртуальный адрес
    uint64_t stack_virt = base_area + (pid * stride);
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

static uint64_t allocate_user_stack(size_t size, uint64_t pml4_phys, uint32_t pid) {
    return allocate_user_stack_in(size, pml4_phys, pid, USER_STACK_AREA_START, USER_STACK_SIZE);
}

// Пишет len байт из src по виртуальному адресу dst_virt в адресном
// пространстве pml4_phys, без переключения CR3 — постранично, через
// get_physical_address_in_pml4()+phys_to_virt() (как elf_load_to_process()
// для сегментов ELF). dst_virt не обязан быть выровнен на страницу.
static void write_user_bytes(uint64_t pml4_phys, uint64_t dst_virt,
                              const void *src, uint64_t len) {
    const uint8_t *s = (const uint8_t*)src;
    uint64_t remaining = len;
    while (remaining > 0) {
        uint64_t page_off = dst_virt & (PAGE_SIZE - 1);
        uint64_t chunk = PAGE_SIZE - page_off;
        if (chunk > remaining) chunk = remaining;

        // get_physical_address_in_pml4() уже возвращает точный (не
        // выровненный) физический адрес байта dst_virt (paging.c: "+ (virt
        // & 0xFFF)"). Прибавлять page_off ещё раз здесь, как делает
        // elf_load_to_process() от выровненного page_va, было бы двойным
        // учётом смещения.
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
    // first_run=1 (настоящий вход в ring3 через context_enter_ring3()) только
    // когда entry==NULL — вызывающий допишет ctx.rip на ELF-адрес позже. Когда
    // entry задан (ядерная функция вроде shell_task), процесс остаётся в
    // ring0 через обычный context_switch() — иначе ring3 попытался бы
    // исполнить код ядра без PAGE_USER.
    proc->first_run = (entry == NULL);
    proc->next_mmap_addr = MMAP_AREA_START;
    memset(proc->mmap_regions, 0, sizeof(proc->mmap_regions));
    memset(proc->signal_handlers, 0, sizeof(proc->signal_handlers));
    proc->pending_signal = 0;
    proc->in_signal_handler = 0;
    proc->alarm_tick = 0;
    proc->cwd_inode = LUFIRAFS_ROOT_INODE;
    proc->cwd_path[0] = '/';
    proc->cwd_path[1] = '\0';
    // В отличие от cwd (всегда на корень), identity наследуется от текущего
    // процесса — иначе run/runbg/exec всегда стартовали бы как root.
    proc->uid = current_process ? current_process->uid : 0;
    proc->gid = current_process ? current_process->gid : 0;
    proc->cpu_ticks = 0;
    mailbox_init(&proc->mailbox);

    // memset перед копированием, не только name[31]=0: иначе хвост буфера
    // за коротким именем остаётся мусором из kmalloc() (не зануляется, см.
    // malloc.c) и "утекает" через код, читающий name дальше NUL (нашлось на
    // SYS_PSLIST: ps.c читал этот хвост как часть имени).
    memset(proc->name, 0, sizeof(proc->name));
    for (int i = 0; i < 31 && name[i]; i++) proc->name[i] = name[i];

    // Свежая fd-таблица (stdin/stdout/stderr на консоль), без наследования
    // родителя — подходит для run/runbg. fork() (process_fork()) отдельно
    // дублирует родительскую таблицу поверх этой.
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
        // уже дала new_pml4 приватную копию identity-map (clone_low_identity_map());
        // bare pmm_free_page() освободил бы только саму страницу PML4.
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

    // Отдельный стек для обработчиков сигналов (см. SIGNAL_STACK_AREA_START,
    // process.h). Не фатально при нехватке памяти: останется 0, и
    // switch_to_process() просто не доставит сигналы этому процессу, как
    // если бы обработчиков не было.
    proc->sig_stack_top = allocate_user_stack_in(SIGNAL_STACK_SIZE, new_pml4, proc->pid,
                                                  SIGNAL_STACK_AREA_START, 0);

    // Инициализируем контекст
    memset(&proc->context, 0, sizeof(process_context_t));

    // Заполняем стек пользователя без переключения CR3 (см. allocate_ring0_stack()):
    // пишем напрямую по физическим адресам, не трогая вызывающий стек.
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

// Ищет процесс, заблокированный в process_wait() именно на этом child
// (по pid или WAIT_ANY_PID). NULL, если такого нет.
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

// Будит родителя child'а, если тот ждёт его в process_wait() (используется
// process_exit() и process_kill()). Возвращает родителя (уже READY) или NULL.
static process_t *wake_waiting_parent(process_t *child) {
    process_t *waiter = find_waiting_parent(child);
    if (!waiter)
        return NULL;

    waiter->wait_target_pid = 0;
    waiter->state = PROCESS_READY;
    return waiter;
}

// Убирает target (уже PROCESS_TERMINATED) из кольцевого списка процессов,
// освобождает его ресурсы (free_process_resources()) и сам process_t.
static void process_remove_from_list(process_t *target) {
    if (!process_list || !target)
        return;

    // process_discard() зовёт это без собственного irq_disable() — таймерный
    // тик мог бы запустить process_reap(), который тоже правит process_list.
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

// Немедленно убирает недостроенный процесс из списка планировщика, например
// если process_fork() не смог доделать ребёнка (см. process.h).
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

    // WM — userspace-процесс; если он сам умирает (крах/kill), снимаем
    // регистрацию (иначе SYS_WIN_*/SYS_FB_* повисли бы, ожидая ответа от
    // несуществующего pid) и восстанавливаем текстовый режим — иначе экран
    // застыл бы на последнем GUI-кадре. Известное ограничение: клиенты, уже
    // заблокированные в wm_call() на RPC к умершему WM, остаются
    // заблокированы навсегда (нет таймаута/сигнала для этого случая).
    if (exiting_process->pid == g_wm_pid) {
        g_wm_pid = 0;
        console_redraw_from_history();
    } else if (g_wm_pid != 0) {
        // Закрытие чужих окон по pid делает сам WM — просто уведомляем его
        // (WM_NOTIFY_PROCESS_EXIT, wm_protocol.h).
        wm_request_t notify;
        memset(&notify, 0, sizeof(notify));
        notify.opcode = WM_NOTIFY_PROCESS_EXIT;
        notify.a[0] = (int32_t)exiting_process->pid;
        mailbox_send(g_wm_pid, WM_SENDER_KERNEL, &notify, sizeof(notify));
    }

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

        // irq_disable() вокруг скана+удаления: process_reap() идёт из
        // timer_irq_handler() на каждом тике и может прилететь посреди
        // обхода process_list, удалив узел, на который мы держим p/p->next.
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
        // Не голый pmm_free_page() — см. process_create(): create_address_space()
        // уже установила приватную copy identity-map.
        free_user_address_space(new_pml4);
        return -1;
    }

    *new_pml4_out = new_pml4;
    *new_stack_out = new_stack;
    return 0;
}

// Подтверждает exec(): переключает proc на подготовленные адресное
// пространство и стек (образ меняется на месте, PID тот же). Старые
// page_table/стек не освобождаются — разбора PML4 в кодовой базе нет
// вообще (process_reap() тоже).
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

    // Новый стек сигналов: старый физически принадлежал заменённому
    // page_table и больше не существует. Не фатально, если не выделился
    // (см. process_create()).
    proc->sig_stack_top = allocate_user_stack_in(SIGNAL_STACK_SIZE, new_pml4, proc->pid,
                                                  SIGNAL_STACK_AREA_START, 0);

    // exec() заменяет всё адресное пространство — старые mmap-регионы
    // физически больше не существуют в new_pml4, сбрасываем учёт.
    proc->next_mmap_addr = MMAP_AREA_START;
    memset(proc->mmap_regions, 0, sizeof(proc->mmap_regions));

    // Обработчики сигналов сбрасываются на default (POSIX: execve() делает
    // то же для пойманных сигналов — их адреса не существуют в новом
    // адресном пространстве).
    memset(proc->signal_handlers, 0, sizeof(proc->signal_handlers));
    proc->pending_signal = 0;
    proc->in_signal_handler = 0;
    proc->alarm_tick = 0;

    // cwd и uid/gid НЕ сбрасываются — POSIX execve() сохраняет текущий
    // каталог и identity процесса (setuid-бит здесь не реализован).

    // memset — как в process_create(): без него, если новое имя короче
    // старого, хвост буфера донёс бы обрывок старого имени.
    memset(proc->name, 0, sizeof(proc->name));
    for (int i = 0; i < 31 && name[i]; i++) proc->name[i] = name[i];

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
    // Должен работать и при IF=0 — важно для process_exit(), syscall
    // handler и interrupt context.
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

    // idle_process сидит в общем кольцевом списке как обычный READY-узел, но
    // этим циклом не выбирается наравне с реальными процессами (у него нет
    // ring0_stack — первый syscall уронил бы систему). Он только запасной
    // вариант ниже, когда больше некого выбрать.
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
        // Больше некого выбрать. current_process ещё RUNNING -> просто
        // продолжаем им же, не переключая на idle (иначе current_process/idle
        // пинг-понговали бы каждый тик). Если же он сам перевёл себя в
        // SLEEPING/BLOCKED/TERMINATED, продолжать им нельзя — единственный
        // кандидат — idle.
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

    // Код после switch_to_process() не выполняется сразу — продолжится,
    // когда старый процесс снова будет выбран scheduler'ом.
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

    // Доставка отложенного сигнала НЕ здесь: context в момент переключения
    // может застать процесс где угодно в середине кернела — подмена
    // context.rip на обработчик привела бы к page fault при резюме (найдено
    // тестированием). Доставка — в syscall_handler()
    // (process_deliver_pending_signal(), перед возвратом из каждого
    // syscall'а), где frame_ptr гарантированно настоящая ring3-точка.

    // Первая активация идёт через настоящий ring0->ring3 переход (iretq,
    // см. process_t.first_run в process.h), не через обычный context_switch()
    // (jmp, CS/CPL не меняются). Все последующие резюме, включая вытесненный
    // из ring3 процесс, используют context_switch().
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

int process_pslist(lufira_ps_entry_t *out, uint32_t max_count) {
    irq_disable();

    uint32_t written = 0;
    process_t *p = process_list;
    if (p) {
        process_t *start = p;
        do {
            if (written >= max_count) break;
            out[written].pid = p->pid;
            out[written].ppid = p->ppid;
            memcpy(out[written].name, p->name, sizeof(out[written].name));
            out[written].state = (uint32_t)p->state;
            out[written].uid = p->uid;
            out[written].cpu_ticks = p->cpu_ticks;
            written++;
            p = p->next;
        } while (p && p != start);
    }

    irq_enable();
    return (int)written;
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

    // v0.8 (GUI+WM), этап 3 - см. тот же приём и комментарий в process_exit() выше.
    if (p->pid == g_wm_pid) {
        g_wm_pid = 0;
        console_redraw_from_history();
    } else if (g_wm_pid != 0) {
        wm_request_t notify;
        memset(&notify, 0, sizeof(notify));
        notify.opcode = WM_NOTIFY_PROCESS_EXIT;
        notify.a[0] = (int32_t)p->pid;
        mailbox_send(g_wm_pid, WM_SENDER_KERNEL, &notify, sizeof(notify));
    }

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

            // SIGINT/SIGTERM/SIGALRM с зарегистрированным обработчиком —
            // откладываем доставку до process_deliver_pending_signal()
            // (syscall.c), не убиваем сразу. SIGKILL всегда default (как в
            // POSIX, не ловится). SIGALRM без обработчика падает в switch()
            // ниже и тоже завершает процесс — тот же POSIX default.
            if ((sig == SIGINT || sig == SIGTERM || sig == SIGALRM) &&
                p->signal_handlers[sig] && !p->in_signal_handler) {
                p->pending_signal = sig;
                // Будим заблокированный/спящий процесс, иначе он никогда не
                // дойдёт до switch_to_process() и не получит сигнал.
                if (p->state == PROCESS_BLOCKED || p->state == PROCESS_SLEEPING)
                    p->state = PROCESS_READY;
                return 0;
            }

            switch (sig) {
                case SIGINT:
                case SIGKILL:
                case SIGTERM:
                case SIGALRM:
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

// См. комментарий у g_wm_pid в process.h.
int process_wm_register(uint32_t pid) {
    if (g_wm_pid != 0) return -1;
    g_wm_pid = pid;
    return 0;
}

uint32_t process_get_wm_pid(void) {
    return g_wm_pid;
}

// Ищет process_t по pid в process_list. NULL, если не найден. Нужно и
// process_set_foreground(), и его обходу вверх по ppid.
process_t* process_find_by_pid(uint32_t pid)
{
    if (!process_list) return NULL;
    process_t *p = process_list;
    process_t *start = p;
    do {
        if (p->pid == pid) return p;
        p = p->next;
    } while (p && p != start);
    return NULL;
}

int process_set_foreground(uint32_t caller_pid, uint32_t target_pid)
{
    if (target_pid == 0) {
        foreground_pid = 0;
        return 0;
    }

    process_t *target = process_find_by_pid(target_pid);
    if (!target) return -1;

    // Поднимаемся по ppid от target, а не сравниваем p->ppid == caller_pid
    // напрямую — так caller может назначить foreground любому потомку, не
    // только прямому ребёнку (см. process.h). depth — защита от цикла в
    // ppid-цепочке (в норме невозможного).
    uint32_t walk_pid = target->ppid;
    int depth = 0;
    while (walk_pid != 0 && depth < MAX_PROCESSES) {
        if (walk_pid == caller_pid) {
            foreground_pid = target_pid;
            return 0;
        }
        process_t *parent = process_find_by_pid(walk_pid);
        if (!parent) break;
        walk_pid = parent->ppid;
        depth++;
    }

    return -1;
}

uint32_t process_get_foreground(void)
{
    return foreground_pid;
}

// Раскладка кадра регистров, который syscall_entry.S сохраняет на ядерном
// стеке перед вызовом syscall_handler(). Передаётся syscall_handler() 7-м
// аргументом — использует только SYS_FORK.
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
    uint64_t user_rsp;  // user RSP на момент этого syscall — на собственном
                        // ядерном стеке процесса, остаётся верным независимо
                        // от того, сколько других процессов сделают свои
                        // syscall'ы, пока этот вызов спит в schedule().
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

    // process_create() уже выделил ребёнку пустое адресное пространство и
    // стек по адресу, вычисленному из его pid — для fork() нужна точная
    // копия адресного пространства родителя, этот плейсхолдер заменяется ниже.
    uint64_t placeholder_pml4 = child->page_table;

    uint64_t new_pml4 = clone_address_space_deep(parent->page_table);
    if (!new_pml4) {
        printf("[FORK] Not enough memory to clone address space\n");
        process_discard(child);
        return (uint64_t)-1;
    }

    child->page_table = new_pml4;
    // Не голый pmm_free_page(): process_create() уже полностью построила
    // этот плейсхолдер (identity-map копия + user-стек) — bare pmm_free_page()
    // освобождал бы только саму страницу PML4, остальное текло бы навсегда.
    free_user_address_space(placeholder_pml4);

    // Ребёнок наследует тот же виртуальный адрес стека, что и родитель —
    // скопирован вместе со всем адресным пространством выше.
    child->stack_base = parent->stack_base;
    child->stack_size = parent->stack_size;

    // process_create() уже завёл ребёнку-плейсхолдеру собственные
    // stdin/stdout/stderr (vfs_init_fd_table()), которые сейчас целиком
    // заменяются родительской таблицей ниже. Без явного закрытия здесь эти
    // 3 file_t/inode никогда не освобождались бы (баг: после ~85 fork()+exec()
    // system-wide file_table[256] забивался осиротевшими записями ino=101-103,
    // и alloc_file() начинал возвращать NULL для настоящих открытий — выглядело
    // как порча ФС). closes временно подменяют current_fd_table на плейсхолдер,
    // как в free_process_resources().
    {
        fd_table_t *saved = current_fd_table;
        current_fd_table = &child->fd_table;
        for (int i = 0; i < MAX_FD_PER_PROCESS; i++) {
            if (current_fd_table->files[i]) vfs_close(i);
        }
        current_fd_table = saved;
    }

    // Родитель и ребёнок получают собственные fd, указывающие на одни и те
    // же file_t/inode (настоящий fork()). vfs_dup_fd() увеличивает
    // inode->ref_count и (для пайпов) readers/writers, чтобы оба будущих
    // независимых vfs_close() учитывались корректно.
    child->fd_table = parent->fd_table;
    for (int i = 0; i < MAX_FD_PER_PROCESS; i++) {
        file_t *f = child->fd_table.files[i];
        if (f) {
            vfs_dup_fd(f);
        }
    }

    // clone_address_space_deep() уже продублировала физические страницы под
    // mmap-регионами родителя — тут только копируем учёт, иначе ребёнок не
    // сможет munmap() унаследованные регионы и затёр бы их своим bump-
    // указателем, начатым заново с MMAP_AREA_START.
    child->next_mmap_addr = parent->next_mmap_addr;
    memcpy(child->mmap_regions, parent->mmap_regions, sizeof(parent->mmap_regions));

    // fork() наследует обработчики сигналов (POSIX), но не текущее
    // исполнение — pending_signal/in_signal_handler/alarm_tick это состояние
    // родителя на момент fork(), ребёнок начинает с чистого листа.
    memcpy(child->signal_handlers, parent->signal_handlers, sizeof(parent->signal_handlers));
    child->pending_signal = 0;
    child->in_signal_handler = 0;
    child->alarm_tick = 0;
    // Тот же виртуальный адрес стека сигналов, что у родителя
    // (SIGNAL_STACK_AREA_START не сдвигается по pid) — страница уже
    // продублирована clone_address_space_deep().
    child->sig_stack_top = parent->sig_stack_top;

    // POSIX: ребёнок наследует cwd и identity родителя 1:1 (нет setuid-бита
    // в этой реализации).
    child->cwd_inode = parent->cwd_inode;
    strcpy(child->cwd_path, parent->cwd_path);
    child->uid = parent->uid;
    child->gid = parent->gid;

    // Ребёнок не выполнялся — счётчик CPU-тиков с нуля.
    child->cpu_ticks = 0;

    // Контекст ребёнка продолжает сразу после syscall в родителе (тот же
    // rip/rflags/callee-saved), но rax=0 — возвращаемое значение fork() для
    // ребёнка. Стартует через обычный context_switch()/jmp, в ring0, как
    // любой новый процесс, до первого syscall'а.
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

// Доставляет p->pending_signal, переписывая конкретный syscall_frame_t,
// который syscall_entry.S восстановит через sysret/iretq (см. вызов в
// syscall_handler(), syscall.c). Единственное безопасное место: в отличие
// от process_t.context (может застать процесс где угодно внутри кернела,
// если вытеснили посреди syscall'а — ранняя версия, подменявшая context
// прямо в switch_to_process(), падала в page fault на резюме), frame_ptr
// здесь всегда настоящая ring3-точка возврата.
void process_deliver_pending_signal(uint64_t frame_ptr) {
    process_t *p = current_process;
    if (!p || !frame_ptr) return;
    // sig_stack_top==0 — выделить стек сигналов не удалось (нехватка
    // памяти) — тихо не доставляем, чем доставлять без годного ring3-стека.
    if (!p->pending_signal || !p->signal_handlers[p->pending_signal] ||
        p->in_signal_handler || !p->sig_stack_top)
        return;

    int sig = p->pending_signal;
    p->pending_signal = 0;
    p->in_signal_handler = 1;

    syscall_frame_t *frame = (syscall_frame_t *)frame_ptr;

    // Сохраняем весь кадр — SYS_SIGRETURN восстановит его целиком
    // (process_sigreturn() ниже).
    process_context_t *saved = &p->saved_signal_context;
    // frame_t не несёт отдельных rcx/r11 — у sysret это слоты user RIP/
    // RFLAGS, захвачены как saved->rip/saved->rflags.
    saved->rax = frame->rax; saved->rbx = frame->rbx;
    saved->rdx = frame->rdx;
    saved->rsi = frame->rsi; saved->rdi = frame->rdi;
    saved->rbp = frame->rbp;
    saved->r8 = frame->r8; saved->r9 = frame->r9; saved->r10 = frame->r10;
    saved->r12 = frame->r12; saved->r13 = frame->r13;
    saved->r14 = frame->r14; saved->r15 = frame->r15;
    saved->rsp = frame->user_rsp;
    saved->rip = frame->rip;
    saved->rflags = frame->rflags;

    frame->rip = p->signal_handlers[sig];
    frame->user_rsp = p->sig_stack_top;
    frame->rdi = (uint64_t)sig; // 1-й аргумент, System V AMD64 ABI
}

// SYS_SIGRETURN — как и SYS_FORK, особый случай (нужен frame_ptr, не
// обычные 5 аргументов). Обязательный способ завершить обработчик сигнала
// (см. sys_signal()/sys_sigreturn(), libc/include/lufira/syscall.h) —
// переносит сохранённый saved_signal_context обратно в syscall_frame_t
// этого вызова, так что sysret возобновит исполнение ровно с места, где
// сигнал застал процесс.
uint64_t process_sigreturn(uint64_t frame_ptr) {
    process_t *p = current_process;
    if (!p || !frame_ptr) return (uint64_t)-1;

    syscall_frame_t *frame = (syscall_frame_t *)frame_ptr;
    process_context_t *saved = &p->saved_signal_context;

    frame->rbx = saved->rbx;
    frame->rdx = saved->rdx;
    frame->rsi = saved->rsi;
    frame->rdi = saved->rdi;
    frame->rbp = saved->rbp;
    frame->r8  = saved->r8;
    frame->r9  = saved->r9;
    frame->r10 = saved->r10;
    frame->r12 = saved->r12;
    frame->r13 = saved->r13;
    frame->r14 = saved->r14;
    frame->r15 = saved->r15;
    frame->rflags = saved->rflags;
    frame->rip = saved->rip;
    frame->user_rsp = saved->rsp;

    p->in_signal_handler = 0;
    // Эпилог syscall_entry.S не восстанавливает rax из кадра (addq $8,%rsp
    // просто пропускает слот) — RAX на выходе это то, что вернула функция,
    // поэтому saved->rax идёт через return, не через frame->rax.
    return saved->rax;
}