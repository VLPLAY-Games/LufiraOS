#include "lib/types.h"
#include "lib/stdarg.h"
#include "lib/cpu.h"
#include "bootinfo.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "system/mm/heap.h"
#include "system/mm/shm.h"
#include "drivers/pci/pci.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/mouse/mouse.h"
#include "drivers/console/console.h"
#include "drivers/serial/serial.h"
#include "drivers/sound/sound.h"
#include "drivers/disk/disk.h"
#include "drivers/usb/xhci.h"
#include "net/net.h"
#include "net/dhcp.h"
#include "system/cpu/gdt.h"
#include "system/cpu/idt.h"
#include "system/cpu/tss.h"
#include "fs/lufirafs/lufirafs.h"
#include "system/cpu/irq.h"
#include "system/acpi/acpi.h"
#include "system/process/process.h"
#include "system/timer/pit.h"
#include "system/timer/hpet.h"
#include "system/timer/rtc.h"
#include "system/syscall/syscall.h"
#include "fs/vfs/vfs.h"
#include "fs/ramfs/ramfs.h"
#include "system/devmode/devmode.h"
#include "system/klog/klog.h"
#include "system/users/users.h"
#include "system/elf/elf.h"
#include "lib/string.h"
#include "log.h"


static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

#define PIC1         0x20
#define PIC2         0xA0
#define PIC1_COMMAND PIC1
#define PIC1_DATA    (PIC1+1)
#define PIC2_COMMAND PIC2
#define PIC2_DATA    (PIC2+1)
#define ICW1_ICW4    0x01
#define ICW1_INIT    0x10

static void pic_remap(void) {
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    outb(PIC1_DATA, 0x20);
    outb(PIC2_DATA, 0x28);
    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    // Ядро целиком на легаси 8259 PIC (Local APIC/IOAPIC не программируются).
    // На части реального железа IMCR (порты 0x22/0x23, Intel MP Spec) держит
    // выходы PIC1 переключёнными на Local APIC вместо CPU — так могла оставить
    // прошивка после multi-core bring-up через APIC. sti/irq_enable(0) в этом
    // случае проходят без ошибок, но IRQ физически не доходит до CPU: первый
    // код, которому нужно реально дождаться тика таймера (xhci_init() ->
    // pit_wait_ms()), виснет намертво. Принудительно переключаем IMCR в
    // "PIC mode" — безопасно и на чипсетах без IMCR (запись игнорируется).
    outb(0x22, 0x70);
    outb(0x23, 0x01);
}

lufirafs_t lufirafs;

// Большой лого-экран на время инициализации (только когда режим
// разработчика выключен — иначе экран занят обычным подробным логом).
static void show_boot_logo(void) {
    clear_entire_screen();

    uint32_t scale = 6;
    const char *title = "LufiraOS";
    uint32_t text_w = text_scaled_width(title, scale);
    uint32_t text_h = (8 + 1) * scale;
    uint32_t x = (screen_width_pixels > text_w) ? (screen_width_pixels - text_w) / 2 : 0;
    uint32_t y = (screen_height_pixels > text_h) ? (screen_height_pixels - text_h) / 2 - 20 : 0;
    draw_text_scaled(title, x, y, scale, RGB_LIGHT_CYAN);

    const char *caption = "Booting...";
    uint32_t cap_w = text_scaled_width(caption, 1);
    uint32_t cap_x = (screen_width_pixels > cap_w) ? (screen_width_pixels - cap_w) / 2 : 0;
    draw_text_scaled(caption, cap_x, y + text_h + 20, 1, RGB_LIGHT_GRAY);
}

// Маленькая наклонная "визитка" сверху терминала — как большое лого при
// загрузке, только компактнее (см. draw_text_tilted() в console.c).
static void draw_shell_watermark(void) {
    draw_text_tilted("LufiraOS", 10, 4, 2, RGB_DARK_GRAY);
}

// Шелл — userspace ELF (/bin/shell.elf), читается СЫРЫМИ примитивами
// LufiraFS (lufirafs_lookup/lufirafs_read), а не через vfs_open(): на
// первом вызове (из _start(), до создания процессов) current_process и
// VFS ещё не существуют. Логика как в elf_exec_internal(background=1)
// (elf.c) для run/runbg, но инлайнена для прямого доступа к process_t*.
static process_t *spawn_shell_process(void) {
    // shell_is_respawn=1: пересоздание шелла после завершения предыдущего
    // (краш/"exit"/Ctrl+C) — флаг взводит respawn_shell_if_needed() в
    // process.c перед вызовом. Баннер не печатаем, чтобы это не выглядело
    // как перезагрузка системы.
    if (shell_is_respawn) {
        shell_is_respawn = 0;
    } else {
        draw_shell_watermark();
        printf("\n\n\n");
        set_foreground_color(LOG_COLOR_HEADER);
        printf("================================================\n");
        printf(" Type 'help' for available commands\n");
        printf("================================================\n\n");
        set_foreground_color(LOG_COLOR_INFO);
    }

    const char *path = "/bin/shell.elf";
    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, lufirafs.sb.root_inode, path, &ino) != 0) {
        printf("[KERNEL] FATAL: %s not found\n", path);
        return NULL;
    }

    lufirafs_inode_t inode;
    if (lufirafs_read_inode(&lufirafs, ino, &inode) != 0 || inode.size == 0) {
        printf("[KERNEL] FATAL: %s unreadable\n", path);
        return NULL;
    }

    uint8_t *buf = (uint8_t *)kmalloc(inode.size);
    if (!buf) {
        printf("[KERNEL] FATAL: out of memory loading %s\n", path);
        return NULL;
    }
    if (lufirafs_read(&lufirafs, ino, 0, buf, inode.size) != (int)inode.size) {
        printf("[KERNEL] FATAL: short read on %s\n", path);
        kfree(buf);
        return NULL;
    }

    process_t *proc = process_create("shell", NULL);
    if (!proc) {
        printf("[KERNEL] FATAL: process_create failed for shell\n");
        kfree(buf);
        return NULL;
    }

    // process_create() всегда ставит cwd в корень — верно для самого
    // первого запуска, но при респауне логичнее унаследовать cwd
    // умиравшего шелла (current_process в этот момент — он же), тем же
    // приёмом, что уже elf_exec_internal() использует для run/runbg.
    if (current_process) {
        proc->cwd_inode = current_process->cwd_inode;
        strcpy(proc->cwd_path, current_process->cwd_path);
    }

    void *entry = elf_load_to_process(buf, inode.size, proc, "shell");
    if (!entry) {
        printf("[KERNEL] FATAL: failed to load %s\n", path);
        proc->state = PROCESS_TERMINATED;
        kfree(buf);
        return NULL;
    }
    proc->context.rip = (uint64_t)entry;

    uint64_t new_rsp, argv_addr, envp_addr;
    if (build_exec_stack(proc->page_table, proc->stack_base, NULL, NULL,
                          &new_rsp, &argv_addr, &envp_addr) != 0) {
        printf("[KERNEL] FATAL: build_exec_stack failed for shell\n");
        proc->state = PROCESS_TERMINATED;
        kfree(buf);
        return NULL;
    }
    proc->context.rsp = new_rsp;
    proc->context.rdi = 0;
    proc->context.rsi = argv_addr;
    proc->context.rdx = envp_addr;

    kfree(buf);
    proc->is_shell = 1;
    return proc;
}

__attribute__((section(".text.prologue")))
void _start(BootInfo* bi) {
    asm volatile ("cli");

    // Чистый опрос портов, никаких зависимостей — нарочно раньше вообще
    // всего остального, чтобы зеркало в serial.c (см. put_char() в
    // console.c) ловило буквально весь консольный вывод, включая самые
    // первые LOG_PENDING/LOG_DONE_OK ниже.
    serial_init();

    initialize_console(bi);

    // Читаем флаг режима разработчика по сырому образу диска ДО lufirafs_init()
    // (см. devmode_probe_early()) — иначе решить, показывать ли лого вместо
    // текстового лога, можно было бы только после монтирования ФС.
    if (bi->FATImageBase && bi->FATImageSize > LUFIRAFS_ESP_SIZE) {
        devmode_probe_early((void*)(bi->FATImageBase + LUFIRAFS_ESP_SIZE),
                             (uint32_t)(bi->FATImageSize - LUFIRAFS_ESP_SIZE));
    }
    if (!devmode_is_enabled()) {
        show_boot_logo();
    }

    LOG_PENDING("Initializing GDT...");
    gdt_init();
    LOG_DONE_OK("GDT initialized");
    
    LOG_PENDING("Initializing TSS...");
    tss_init();
    LOG_DONE_OK("TSS initialized");
    
    LOG_PENDING("Initializing IDT...");
    idt_init();
    LOG_DONE_OK("IDT initialized");
    
    LOG_PENDING("Remapping PIC...");
    pic_remap();
    LOG_DONE_OK("PIC remapped");

    // Порядок важен: сначала PMM, потом PAGING, потом HEAP.
    // reserved_base/reserved_size покрывают весь диск, загруженный бутлоадером
    // в RAM одним куском (bi->FATImageBase, см. fat_loader.c) — зачем это
    // резервировать, см. подробности у pmm_init() в pmm.c.
    pmm_init(bi->MemoryMap, bi->MemoryMapSize, bi->MemoryMapDescriptorSize,
                bi->KernelBase, bi->KernelSize,
                bi->FATImageBase, bi->FATImageSize);
    paging_init(bi);
    
    // Heap теперь статический - инициализируем сразу
    heap_init();  // <-- ВСЯ память выделяется здесь
    shm_init();   // v0.8-мост, пункт 4: реестр MAP_SHARED областей (shm.c)
    // v0.8 (GUI+WM), этап 3: отдельной gui_init() больше нет — таблица окон
    // переехала в userspace WM-процесс (lufira-packages/src/apps/wm.c),
    // ядру тут нечего инициализировать (g_wm_pid/mailbox уже по умолчанию
    // 0/пустой, см. process.c).

    // Бутлоадер грузит весь диск в RAM одним куском с LBA 0: первые
    // LUFIRAFS_ESP_SIZE байт — FAT-раздел (ESP) прошивки, LufiraFS начинается
    // сразу за ним. lba_offset нужен lufirafs_sync(), чтобы писать "грязные"
    // блоки по правильным абсолютным LBA через drivers/disk (см. lufirafs.c).
    if (bi->FATImageBase && bi->FATImageSize > LUFIRAFS_ESP_SIZE) {
        LOG_PENDING("Mounting LufiraFS...");
        void *fs_image = (void*)(bi->FATImageBase + LUFIRAFS_ESP_SIZE);
        uint32_t fs_size = (uint32_t)(bi->FATImageSize - LUFIRAFS_ESP_SIZE);
        uint32_t lba_offset = LUFIRAFS_ESP_SIZE / 512;
        if (lufirafs_init(&lufirafs, fs_image, fs_size, lba_offset) == 0) {
            LOG_DONE_OK("LufiraFS mounted");
            devmode_init();
            klog_init();
            users_init();
            if (users_root_has_default_password()) {
                printf("[SECURITY] root password is still the default - run 'passwd -u root <new>' to change it\n");
            }
            klog("[BOOT] LufiraOS booting, devmode=%u", devmode_is_enabled());
        } else {
            LOG_DONE_FAIL("LufiraFS mount failed");
        }
    } else {
        LOG_FAIL("No disk image provided");
    }

    if (bi->RsdpAddress) {
        LOG_PENDING("Initializing ACPI...");
        if (acpi_init(bi->RsdpAddress) == 0) {
            LOG_DONE_OK("ACPI initialized");
        } else {
            LOG_DONE_WARN("ACPI initialization failed");
        }
    } else {
        LOG_WARN("No RSDP found, ACPI disabled");
    }

    LOG_PENDING("Initializing process manager...");
    process_init();
    LOG_DONE_OK("Process manager initialized");

    LOG_PENDING("Initializing PIT...");
    pit_init();
    LOG_DONE_OK("PIT initialized at %d Hz", PIT_FREQUENCY);

    // HPET требует уже распарсенного ACPI (таблица "HPET") — опциональный,
    // более точный источник времени поверх PIT, не замена: scheduler-тики
    // (IRQ0) всё равно только PIT.
    LOG_PENDING("Initializing HPET...");
    hpet_init();
    if (hpet_available()) {
        LOG_DONE_OK("HPET initialized (%lu Hz)", hpet_ticks_per_second());
    } else {
        LOG_DONE_WARN("HPET not available on this platform");
    }

    LOG_PENDING("Reading RTC...");
    rtc_time_t boot_time;
    if (rtc_get_time(&boot_time) == 0) {
        // Этот printf (console.c) не умеет ширину/нулевое дополнение
        // (%02u) — та же причина, по которой date.c (lufira-packages)
        // дополняет сам: без ведущих нулей, но корректно.
        LOG_DONE_OK("RTC: %u-%u-%u %u:%u:%u UTC",
                     boot_time.year, boot_time.month, boot_time.day,
                     boot_time.hour, boot_time.minute, boot_time.second);
    } else {
        LOG_DONE_WARN("RTC read failed");
    }

    LOG_PENDING("Initializing syscalls...");
    syscall_init();
    LOG_DONE_OK("Syscalls initialized");

    LOG_PENDING("Initializing VFS...");
    vfs_init();
    LOG_DONE_OK("VFS initialized");

    // v0.9, фаза 1 — RAMFS: монтируем /tmp по умолчанию, тем же способом,
    // каким пользователь монтировал бы любой другой RAMFS-инстанс
    // (ramfs_mount(), fs/ramfs/ramfs.h) — не особый случай в самом RAMFS.
    LOG_PENDING("Mounting /tmp (ramfs)...");
    if (ramfs_mount("/tmp") == 0) {
        LOG_DONE_OK("/tmp mounted (ramfs)");
    } else {
        LOG_DONE_WARN("/tmp (ramfs) mount failed");
    }

    // pcspeaker_init();

    pci_init();

    asm volatile("sti");
    irq_enable(0);  // таймер
    irq_enable(2);  // cascade (нужен для IRQ8-15, в т.ч. мыши — IRQ12)
    cpu_mark_interrupts_active();

    // Двойную буферизацию включаем здесь, а не в initialize_console() —
    // именно ПОСЛЕ sti/irq_enable(0), чтобы таймер уже тикал и
    // console_tick_present() (из pit_timer_handler()) сразу флашил back
    // buffer без слепого окна. До этой точки вывод шёл прямо в hw-буфер.
    console_enable_double_buffering();

    // disk_init() пробует AHCI/NVMe (ahci.c/nvme.c) перед легаси ATA PIO.
    // Контроллеры ждут готовности busy-wait циклами с фиксированным числом
    // итераций MMIO-чтения — под TCG-эмуляцией это превращается в заметную
    // паузу (иногда и в ложный "контроллер не ответил"). Поэтому disk_init(),
    // как и xhci_init() ниже, вызывается ПОСЛЕ sti/irq_enable(0): ahci.c/
    // nvme.c используют pit_wait_ms() (откалиброван по тикам PIT) вместо
    // голого спина.
    //
    // klog_init()/klog() выше (при монтировании LufiraFS) пишут на диск ещё
    // ДО этой точки, то есть ДО sti, тем же legacy ATA PIO путём (ahci/nvme
    // пока не инициализированы). Поэтому ata_wait_ready()/ata_wait_drq() в
    // disk.c проверяют cpu_interrupts_active() перед вызовом pit_wait_ms(),
    // а не предполагают, что прерывания уже включены.
    disk_init();

    // Требует уже тикающего таймера (pit_wait_ms() внутри сброса контроллера).
    //
    // xHCI (USB-клавиатура/мышь, usb_hid.c) инициализируется ПЕРЕД легаси
    // PS/2 — это более вероятный источник реального HID, PS/2 остаётся лишь
    // фоллбэком. Безлимитный busy-wait PS/2 на отсутствующем устройстве
    // больше не вешает загрузку независимо от порядка — см. таймауты
    // PS2_TIMEOUT_ITERS в mouse.c.
    xhci_init();

    LOG_PENDING("Initializing keyboard...");
    keyboard_init();
    irq_enable(1);  // клавиатура (PS/2) — включаем маску именно тут, не раньше
    LOG_DONE_OK("Keyboard %s", keyboard_is_initialized() ? "ready" : "not found");

    LOG_PENDING("Initializing mouse...");
    mouse_init();
    irq_enable(12); // мышь (PS/2)
    LOG_DONE_OK("Mouse %s", mouse_is_initialized() ? "ready" : "not found");

    // net_init() (RTL8139/E1000/E1000E + статическая настройка IP как
    // фоллбэк) — по той же причине, что и xhci_init(), требует уже
    // тикающих таймерных прерываний.
    net_init();

    // Пробуем получить DHCP-аренду (IP/маска/шлюз/DNS), если в сети есть
    // сервер (включая QEMU SLIRP); при неудаче net_get_config() остаётся на
    // статическом дефолте из net.c — тот же принцип деградации, что и у
    // остального сетевого стека. ~4с бюджет короче HTTP_TOTAL_BUDGET_TICKS
    // (http_client.c), чтобы не раздувать время загрузки без DHCP-сервера.
    if (net_found()) {
        if (dhcp_start(4000) == 0) {
            DLOG("[DHCP] lease applied\n");
        } else {
            DLOG("[DHCP] no lease, staying on static default\n");
        }
    }

    // Пробует HDA (hda.c) первым, затем AC'97 (ac97.c) — тот же диспетчер,
    // что у AHCI/NVMe выше (см. sound.c). Требует тикающего таймера
    // (pit_wait_ms() в CORB/RIRB-обмене и сбросе контроллера), поэтому
    // вызывается после sti — см. разбор в hda.c.
    if (sound_init() == 0) {
        DLOG("[ OK ] Audio ready (%s)\n", sound_driver_name());
    } else {
        printf("[WARN] No audio controller available\n");
    }

    if (devmode_is_enabled()) {
        printf("\n");
        set_foreground_color(LOG_COLOR_HEADER);
        printf("================================================\n");
        printf("     LufiraOS Kernel v0.9.0                     \n");
        printf("================================================\n");
        set_foreground_color(LOG_COLOR_INFO);

        printf("\n");
        set_foreground_color(LOG_COLOR_HEADER);
        printf("SYSTEM STATUS:\n");

        LOG_STATUS_LINE("Console", 1, "READY");
        LOG_STATUS_LINE("Keyboard", keyboard_is_initialized(), keyboard_is_initialized() ? "READY" : "NOT FOUND");
        LOG_STATUS_LINE("Mouse", mouse_is_initialized(), mouse_is_initialized() ? "READY" : "NOT FOUND");
        LOG_STATUS_LINE("Syscalls", 1, "ACTIVE (75 syscalls)");
        LOG_STATUS_LINE("VFS", 1, "READY");

        set_foreground_color(STATUS_READY);
        printf("  Memory manager: INITIALIZED\n");
        // Планировщик вытесняет ring3-код по таймеру (PREEMPT_TIMESLICE_TICKS,
        // pit.c) — строка отражает текущее поведение, не историческое "COOPERATIVE".
        printf("  Scheduler: PREEMPTIVE\n");
        printf("  Process manager: INITIALIZED\n");
        printf("  LufiraFS: %s\n", lufirafs_mounted ? "MOUNTED" : "NOT MOUNTED");
        printf("  Developer mode: %s\n", devmode_is_enabled() ? "ON" : "OFF");
        set_foreground_color(LOG_COLOR_INFO);
    } else {
        clear_entire_screen();
    }

    process_set_shell_spawner(spawn_shell_process);
    process_t *shell_proc = spawn_shell_process();
    if (!shell_proc) {
        printf("[KERNEL] FATAL: could not start /bin/shell.elf — halting\n");
        while (1) asm volatile("hlt");
    }

    while (1) {
        asm volatile("sti");
        asm volatile("hlt");
        asm volatile("cli");
        schedule();
        // Сбор осиротевших зомби (process_reap()) сделан прямо в
        // timer_irq_handler() (pit.c), а не здесь: schedule() (process.c)
        // намеренно почти никогда не переключает управление на сам
        // idle-процесс, пока жив хоть один другой READY-процесс (шелл жив
        // всегда) — так что этот цикл реально исполняется довольно редко,
        // и вызов отсюда был бы ненадёжен.
    }
}