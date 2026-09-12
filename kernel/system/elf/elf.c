#include "elf.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "system/mm/heap.h"
#include "system/process/process.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"
#include "lib/string.h"

#ifndef PAGE_PS
#define PAGE_PS     0x80    // Page size (2MB / 1GB)
#endif

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

int elf_validate(const elf64_header_t *header) {
    if (header->magic != ELF_MAGIC) {
        printf("[ELF] Invalid magic: 0x%x\n", header->magic);
        return -1;
    }
    
    if (header->elf_class != ELFCLASS64) {
        printf("[ELF] Not a 64-bit executable\n");
        return -1;
    }
    
    if (header->machine != EM_X86_64) {
        printf("[ELF] Not an x86-64 executable\n");
        return -1;
    }
    
    if (header->type != ET_EXEC && header->type != ET_DYN) {
        printf("[ELF] Not an executable (type=%u)\n", header->type);
        return -1;
    }
    
    if (header->phnum == 0) {
        printf("[ELF] No program headers\n");
        return -1;
    }
    
    return 0;
}

/*
 * Выделение страницы и маппинг в УКАЗАННОМ адресном пространстве (по pml4_phys).
 * НЕ переключает CR3 - работает через identity mapping для доступа к таблицам.
 * 
 * ВАЖНО: Эта функция модифицирует таблицы страниц по указанному pml4_phys.
 * Вызывающий код должен обеспечить, что:
 * 1. Текущий CR3 позволяет через identity mapping читать/писать все 
 *    физические адреса (как выделяемые pmm_alloc_page, так и сам pml4_phys).
 * 2. pml4_phys валиден и указывает на PML4 целевого адресного пространства.
 */
// ============================================================
// ИСПРАВЛЕННАЯ map_page_in_space()
// ============================================================

static int map_page_in_space(uint64_t pml4_phys,
                             uint64_t virt,
                             uint64_t phys,
                             uint64_t flags)
{
    uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    /*
     * Все промежуточные таблицы user mapping должны иметь USER=1.
     */
    const uint64_t table_flags =
        PAGE_PRESENT |
        PAGE_WRITE |
        PAGE_USER;

    /*
     * PML4 -> PDPT
     */
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {

        uint64_t new_pdpt_phys = pmm_alloc_page();
        if (!new_pdpt_phys)
            return -1;

        uint64_t *new_pdpt = (uint64_t *)phys_to_virt(new_pdpt_phys);

        for (int i = 0; i < 512; i++)
            new_pdpt[i] = 0;

        pml4[pml4_idx] =
            (new_pdpt_phys & 0x000FFFFFFFFFF000ULL) |
            table_flags;
    } else {
        /*
         * Для user mapping existing PML4 entry обязан быть USER.
         */
        pml4[pml4_idx] |= PAGE_USER;
    }

    uint64_t *pdpt =
        (uint64_t *)phys_to_virt(pml4[pml4_idx] & 0x000FFFFFFFFFF000ULL);

    /*
     * PDPT -> PD
     */
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {

        uint64_t new_pd_phys = pmm_alloc_page();
        if (!new_pd_phys)
            return -1;

        uint64_t *new_pd = (uint64_t *)phys_to_virt(new_pd_phys);

        for (int i = 0; i < 512; i++)
            new_pd[i] = 0;

        pdpt[pdpt_idx] =
            (new_pd_phys & 0x000FFFFFFFFFF000ULL) |
            table_flags;
    } else {
        pdpt[pdpt_idx] |= PAGE_USER;
    }

    uint64_t *pd =
        (uint64_t *)phys_to_virt(pdpt[pdpt_idx] & 0x000FFFFFFFFFF000ULL);

    /*
     * Если существовала huge page, разбиваем её.
     */
    if (pd[pd_idx] & PAGE_PS) {

        uint64_t huge_entry = pd[pd_idx];

        uint64_t phys_2m =
            huge_entry & 0x000FFFFFFFE00000ULL;

        uint64_t orig_flags =
            huge_entry & 0xFFFULL;

        orig_flags &= ~PAGE_PS;

        uint64_t new_pt_phys = pmm_alloc_page();
        if (!new_pt_phys)
            return -1;

        uint64_t *new_pt = (uint64_t *)phys_to_virt(new_pt_phys);

        for (int j = 0; j < 512; j++) {
            new_pt[j] =
                (phys_2m + j * PAGE_SIZE) |
                orig_flags |
                PAGE_PRESENT |
                PAGE_USER;
        }

        /*
         * Сам PDE тоже должен быть USER.
         */
        pd[pd_idx] =
            (new_pt_phys & 0x000FFFFFFFFFF000ULL) |
            PAGE_PRESENT |
            PAGE_WRITE |
            PAGE_USER;
    }

    /*
     * PD -> PT
     */
    if (!(pd[pd_idx] & PAGE_PRESENT)) {

        uint64_t new_pt_phys = pmm_alloc_page();
        if (!new_pt_phys)
            return -1;

        uint64_t *new_pt = (uint64_t *)phys_to_virt(new_pt_phys);

        for (int i = 0; i < 512; i++)
            new_pt[i] = 0;

        pd[pd_idx] =
            (new_pt_phys & 0x000FFFFFFFFFF000ULL) |
            table_flags;
    } else {
        /*
         * Для user page PDE обязан иметь USER.
         */
        pd[pd_idx] |= PAGE_USER;
    }

    uint64_t *pt =
        (uint64_t *)phys_to_virt(pd[pd_idx] & 0x000FFFFFFFFFF000ULL);

    /*
     * NX находится в bit 63, поэтому его нельзя терять
     * через (flags & 0xFFF).
     */
    uint64_t pte_flags =
        (flags & 0xFFFULL) |
        (flags & PAGE_NX);

    pt[pt_idx] =
        (phys & 0x000FFFFFFFFFF000ULL) |
        pte_flags |
        PAGE_PRESENT;

    return 0;
}

// ============================================================
// НОВАЯ set_page_flags_in_space()
// ============================================================

static int set_page_flags_in_space(uint64_t pml4_phys,
                                   uint64_t virt,
                                   uint64_t flags)
{
    uint64_t *pml4 = (uint64_t*)phys_to_virt(pml4_phys);

    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & PAGE_PRESENT))
        return -1;

    uint64_t *pdpt =
        (uint64_t*)phys_to_virt(pml4[pml4_idx] & 0x000FFFFFFFFFF000ULL);

    if (!(pdpt[pdpt_idx] & PAGE_PRESENT))
        return -1;

    uint64_t *pd =
        (uint64_t*)phys_to_virt(pdpt[pdpt_idx] & 0x000FFFFFFFFFF000ULL);

    if (!(pd[pd_idx] & PAGE_PRESENT))
        return -1;

    if (pd[pd_idx] & PAGE_PS)
        return -1;

    uint64_t *pt =
        (uint64_t*)phys_to_virt(pd[pd_idx] & 0x000FFFFFFFFFF000ULL);

    if (!(pt[pt_idx] & PAGE_PRESENT))
        return -1;

    uint64_t phys = pt[pt_idx] & 0x000FFFFFFFFFF000ULL;

    // NX — бит 63, теряется через (flags & 0xFFF) — берём его отдельно,
    // как и в map_page_in_space() выше.
    pt[pt_idx] =
        phys |
        (flags & 0xFFFULL) |
        (flags & PAGE_NX) |
        PAGE_PRESENT;

    return 0;
}

// Загрузка ELF в адресное пространство процесса
void* elf_load_to_process(const void *elf_data,
                          uint64_t elf_size,
                          process_t *proc,
                          const char *name)
{
    if (!elf_data || !elf_size || !proc) {
        printf("[ELF] Invalid parameters\n");
        return NULL;
    }

    const elf64_header_t *header =
        (const elf64_header_t*)elf_data;

    if (elf_validate(header) != 0)
        return NULL;

    printf("[ELF] Loading '%s' into process %u\n",
           name,
           proc->pid);

    uint64_t proc_pml4 = proc->page_table;

    const elf64_program_header_t *ph =
        (const elf64_program_header_t*)
        ((const uint8_t*)elf_data + header->phoff);

    typedef struct {
        uint64_t vaddr;
        uint64_t memsz;
        uint64_t filesz;
        uint64_t offset;
        uint32_t flags;
    } segment_info_t;

    segment_info_t segments[32];
    int seg_count = 0;

    // ========================================================
    // СОБИРАЕМ PT_LOAD
    // ========================================================

    for (int i = 0; i < header->phnum; i++) {

        if (ph[i].type != PT_LOAD)
            continue;

        segments[seg_count].vaddr  = ph[i].vaddr;
        segments[seg_count].memsz  = ph[i].memsz;
        segments[seg_count].filesz = ph[i].filesz;
        segments[seg_count].offset = ph[i].offset;
        segments[seg_count].flags  = ph[i].flags;

        seg_count++;
    }

    // ========================================================
    // МАППИНГ
    // ВАЖНО:
    // ВСЕ СТРАНИЦЫ ВРЕМЕННО WRITABLE
    // ========================================================

    for (int i = 0; i < seg_count; i++) {

        uint64_t seg_start =
            segments[i].vaddr & ~(PAGE_SIZE - 1);

        uint64_t seg_end =
            (segments[i].vaddr +
             segments[i].memsz +
             PAGE_SIZE - 1)
            & ~(PAGE_SIZE - 1);

        uint64_t seg_pages =
            (seg_end - seg_start) / PAGE_SIZE;

        printf("[ELF] Segment %d: 0x%lx - 0x%lx (%lu pages)\n",
               i,
               seg_start,
               seg_end,
               seg_pages);

        for (uint64_t p = 0; p < seg_pages; p++) {

            uint64_t virt =
                seg_start + p * PAGE_SIZE;

            uint64_t phys =
                pmm_alloc_page();

            if (!phys) {
                printf("[ELF] Out of memory\n");
                return NULL;
            }

            // ============================================
            // ВРЕМЕННО WRITE ДЛЯ ВСЕХ
            // ============================================

            uint64_t flags =
                PAGE_USER |
                PAGE_WRITE;

            if (!(segments[i].flags & PF_X))
                flags |= PAGE_NX;

            if (map_page_in_space(proc_pml4,
                                  virt,
                                  phys,
                                  flags) != 0)
            {
                printf("[ELF] map failed\n");
                pmm_free_page(phys);
                return NULL;
            }
        }
    }

    // ========================================================
    // ZERO + COPY — постранично, по физическим адресам, БЕЗ
    // переключения CR3.
    //
    // Раньше эта функция переключала CR3 на proc_pml4 и писала через
    // виртуальные адреса ОДНИМ большим memset()/memcpy() на весь
    // сегмент — но segments[] лежит локальным массивом на стеке
    // ВЫЗЫВАЮЩЕГО кода (elf_load_to_process вызывается из process_create,
    // обычно прямо из keyboard_irq_handler — а он выполняется на
    // пользовательском стеке ВЫЗЫВАЮЩЕГО процесса, чей PML4-индекс в
    // свежесозданном proc_pml4 отсутствует), плюс сам elf_data — указатель
    // на буфер вызывающего. Переключение CR3 мгновенно обрывало этот
    // стек. См. подробное объяснение у allocate_ring0_stack() в process.c.
    // ========================================================

    for (int i = 0; i < seg_count; i++) {

        uint64_t seg_start =
            segments[i].vaddr & ~(PAGE_SIZE - 1);

        uint64_t seg_end =
            (segments[i].vaddr +
             segments[i].memsz +
             PAGE_SIZE - 1)
            & ~(PAGE_SIZE - 1);

        // ================================================
        // ZERO — постранично
        // ================================================

        for (uint64_t va = seg_start; va < seg_end; va += PAGE_SIZE) {
            uint64_t phys = get_physical_address_in_pml4(proc_pml4, va);
            if (phys) memset(phys_to_virt(phys), 0, PAGE_SIZE);
        }

        // ================================================
        // COPY — постранично (начало/конец сегмента могут не совпадать
        // с границами страниц)
        // ================================================

        if (segments[i].filesz > 0) {
            uint64_t dst_vaddr = segments[i].vaddr;
            uint64_t remaining = segments[i].filesz;
            const uint8_t *src = (const uint8_t*)elf_data + segments[i].offset;

            while (remaining > 0) {
                uint64_t page_va = dst_vaddr & ~(PAGE_SIZE - 1);
                uint64_t page_off = dst_vaddr - page_va;
                uint64_t chunk = PAGE_SIZE - page_off;
                if (chunk > remaining) chunk = remaining;

                uint64_t phys = get_physical_address_in_pml4(proc_pml4, page_va);
                if (phys) memcpy((uint8_t*)phys_to_virt(phys) + page_off, src, chunk);

                dst_vaddr += chunk;
                src += chunk;
                remaining -= chunk;
            }
        }
    }

    // ========================================================
    // ВОЗВРАЩАЕМ ФИНАЛЬНЫЕ ПРАВА (set_page_flags_in_space уже работает
    // по физическому pml4_phys, CR3 не трогает)
    // ========================================================

    for (int i = 0; i < seg_count; i++) {

        uint64_t seg_start =
            segments[i].vaddr & ~(PAGE_SIZE - 1);

        uint64_t seg_end =
            (segments[i].vaddr +
             segments[i].memsz +
             PAGE_SIZE - 1)
            & ~(PAGE_SIZE - 1);

        uint64_t seg_pages =
            (seg_end - seg_start) / PAGE_SIZE;

        uint64_t final_flags =
            PAGE_USER;

        if (segments[i].flags & PF_W)
            final_flags |= PAGE_WRITE;

        if (!(segments[i].flags & PF_X))
            final_flags |= PAGE_NX;

        for (uint64_t p = 0; p < seg_pages; p++) {

            uint64_t virt =
                seg_start + p * PAGE_SIZE;

            set_page_flags_in_space(
                proc_pml4,
                virt,
                final_flags
            );
        }
    }

    printf("[ELF] Loaded successfully\n");

    return (void*)header->entry;
}

// Сохраняет текущее EFLAGS.IF и отключает прерывания. Команды шелла
// (run/exec) выполняются синхронно прямо из обработчика IRQ1 клавиатуры
// (см. комментарий про exec ниже), где прерывания уже аппаратно отключены
// самим CPU — безусловный "sti" на ранних return'ах ниже раньше
// ПРЕЖДЕВРЕМЕННО включал их посреди создания процесса, позволяя
// таймерному IRQ (который теперь ещё и опрашивает USB) вклиниться прямо в
// этот момент и повредить кучу/список процессов. Восстанавливаем именно
// то состояние, что было на входе, а не форсируем "включено".
static inline uint64_t elf_irq_save(void) {
    uint64_t flags;
    asm volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    asm volatile("cli");
    return flags;
}

static inline void elf_irq_restore(uint64_t flags) {
    asm volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

// Загрузка ELF и создание процесса
static int elf_exec_internal(const void *elf_data,
                             uint64_t elf_size,
                             const char *name,
                             int background)
{
    uint64_t irq_flags = elf_irq_save();

    // Создаём процесс
    process_t *proc = process_create(name, NULL);
    if (!proc) {
        printf("[ELF] Failed to create process\n");
        elf_irq_restore(irq_flags);
        kfree((void*)elf_data);
        return -1;
    }

    // Загружаем ELF
    void *entry = elf_load_to_process(
        elf_data,
        elf_size,
        proc,
        name
    );

    if (!entry) {
        printf("[ELF] Failed to load ELF\n");
        proc->state = PROCESS_TERMINATED;
        kfree((void*)elf_data);
        elf_irq_restore(irq_flags);
        return -1;
    }

    // Устанавливаем точку входа
    proc->context.rip = (uint64_t)entry;

    // Освобождаем буфер ELF (данные уже скопированы)
    kfree((void*)elf_data);

    if (background) {
        proc->state = PROCESS_READY;
        printf("[ELF] Background process ready: PID %u\n", proc->pid);
        elf_irq_restore(irq_flags);
        return (int)proc->pid;
    }

    // FOREGROUND
    asm volatile("cli");

    // Отсюда мы прыгаем в новый процесс сырым context_switch() и можем
    // не вернуться в этот стек вызовов ещё очень долго (пока proc сам не
    // уступит CPU) — значит обычный send_eoi() в irq_handler(), который
    // выполнился бы ПОСЛЕ штатного возврата из текущего обработчика IRQ,
    // не выполнится вовсе. В этом ядре ЛЮБАЯ команда шелла (run/exec/...)
    // может быть вызвана не только из PS/2 keyboard_irq_handler() (IRQ1),
    // но и из timer_irq_handler() (IRQ0) — потому что USB HID-клавиатура
    // опрашивается через usb_poll() прямо оттуда, а QEMU обычно доставляет
    // один и тот же keystroke сразу на PS/2 и на USB устройство. Если
    // "Enter" был обработан именно через USB-путь, "потерянный" EOI — это
    // EOI САМОГО ТАЙМЕРА: PIC считает IRQ0 бесконечно "в обслуживании",
    // и ни один следующий таймерный тик больше никогда не доставляется —
    // весь планировщик виснет намертво (наблюдалось: EFLAGS.IF=1, HLT=1,
    // но `info pic` показывает isr=01 для IRQ0). Шлём EOI на оба PIC
    // вручную и безусловно (это no-op, если реально нечего подтверждать)
    // — какой бы IRQ ни привёл нас сюда.
    outb(0xA0, 0x20);
    outb(0x20, 0x20);

    // Цель для Ctrl+C (см. shell_handle_ctrl_c() в shell.c). runbg сюда не
    // попадает вовсе (см. ранний return в background-ветке выше) — фоновые
    // процессы Ctrl+C не прерывает, как и в настоящих шеллах.
    foreground_pid = proc->pid;

    switch_to_process(proc);

    return 0;
}

int elf_exec(const void *elf_data,
             uint64_t elf_size,
             const char *name)
{
    return elf_exec_internal(
        elf_data,
        elf_size,
        name,
        0
    );
}

int elf_exec_background(const void *elf_data,
                        uint64_t elf_size,
                        const char *name)
{
    return elf_exec_internal(
        elf_data,
        elf_size,
        name,
        1
    );
}

// Настоящий execve(): заменяет ОБРАЗ текущего процесса (например, shell)
// программой из elf_data, вместо того чтобы создавать отдельный новый
// процесс и переключаться на него. PID, ring0-стек и место процесса в
// списке планировщика не меняются — управление просто больше никогда не
// возвращается к старому коду процесса.
int elf_exec_replace(const void *elf_data, uint64_t elf_size, const char *name)
{
    uint64_t irq_flags = elf_irq_save();

    process_t *proc = current_process;
    if (!proc) {
        elf_irq_restore(irq_flags);
        kfree((void*)elf_data);
        return -1;
    }

    uint64_t new_pml4, new_stack;
    if (process_prepare_exec(proc, &new_pml4, &new_stack) != 0) {
        printf("[ELF] exec: not enough memory for new address space\n");
        elf_irq_restore(irq_flags);
        kfree((void*)elf_data);
        return -1;
    }

    // Грузим ELF во ВРЕМЕННЫЙ локальный дескриптор с новым pml4, чтобы не
    // трогать текущий (ещё рабочий) образ proc, пока не убедимся, что
    // новая программа загрузилась успешно.
    process_t shadow = *proc;
    shadow.page_table = new_pml4;

    void *entry = elf_load_to_process(elf_data, elf_size, &shadow, name);

    kfree((void*)elf_data);

    if (!entry) {
        printf("[ELF] exec: failed to load '%s', old process untouched\n", name);
        elf_irq_restore(irq_flags);
        return -1;
    }

    // Готовим начальный кадр пользовательского стека новой программы —
    // так же, как это делает process_create() для только что созданного
    // процесса: возврат "с конца" main() уводит в process_exit().
    //
    // Пишем по физическому адресу, не переключая CR3 — see подробное
    // объяснение у allocate_ring0_stack() в process.c: текущий стек
    // вызовов (exec/do_exec/shell/...) лежит на пользовательском стеке
    // ЭТОГО ЖЕ процесса, чей PML4-индекс в new_pml4 (свежесозданном
    // адресном пространстве) ещё отсутствует.
    uint64_t rsp = new_stack;
    rsp -= 8;
    uint64_t rsp_phys = get_physical_address_in_pml4(new_pml4, rsp);
    *(uint64_t*)phys_to_virt(rsp_phys) = (uint64_t)process_exit;

    process_commit_exec(proc, new_pml4, new_stack, name);

    process_context_t *ctx = &proc->context;
    memset(ctx, 0, sizeof(*ctx));
    ctx->rsp = rsp;
    ctx->rip = (uint64_t)entry;
    ctx->rflags = 0x202;
    ctx->cr3 = new_pml4;

    printf("[ELF] Process %u replaced with '%s' (entry=0x%lx)\n",
           proc->pid, name, (uint64_t)entry);

    // Отсюда мы уже никогда не вернёмся по этому стеку вызовов, а shell-
    // команда "exec" обычно вызывается прямо из обработчика IRQ (PS/2
    // клавиатура — IRQ1, но и таймер — IRQ0, поскольку USB HID-клавиатура
    // опрашивается через usb_poll() прямо из timer_irq_handler(), а QEMU
    // обычно доставляет один и тот же keystroke сразу на оба устройства),
    // и штатный send_eoi() в irq_handler() для этого прерывания не
    // выполнится. Подтверждаем вручную на обоих PIC (безусловно — если
    // подтверждать нечего, это no-op) — иначе PIC считает соответствующий
    // IRQ "в обслуживании" навсегда; для IRQ0 это означает, что ни один
    // следующий таймерный тик больше никогда не доставляется и весь
    // планировщик виснет намертво.
    outb(0xA0, 0x20);
    outb(0x20, 0x20);

    // Цель для Ctrl+C (см. shell_handle_ctrl_c() в shell.c) — exec меняет
    // образ proc "на месте", PID остаётся тем же.
    foreground_pid = proc->pid;

    // Прыгаем в новый образ процесса и не возвращаемся: старый контекст
    // (стек вызовов exec/do_exec/shell/...) сохранять некуда и незачем —
    // это и есть "замена", а не создание нового процесса.
    process_context_t discard;
    context_switch(&discard, ctx);

    __builtin_unreachable();
}