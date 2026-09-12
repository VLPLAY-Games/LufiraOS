#include "lib/types.h"
#include "lib/stdarg.h"
#include "bootinfo.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "system/mm/heap.h"
#include "drivers/pci/pci.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/mouse/mouse.h"
#include "drivers/console/console.h"
#include "drivers/sound/ac97.h"
#include "drivers/usb/uhci.h"
#include "shell/shell.h"
#include "system/cpu/gdt.h"
#include "system/cpu/idt.h"
#include "system/cpu/tss.h"
#include "fs/lufirafs/lufirafs.h"
#include "system/cpu/irq.h"
#include "system/acpi/acpi.h"
#include "system/process/process.h"
#include "system/timer/pit.h"
#include "system/syscall/syscall.h"
#include "fs/vfs/vfs.h"
#include "system/devmode/devmode.h"
#include "system/klog/klog.h"
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
}

lufirafs_t lufirafs;

static void shell_task(void) {
    printf("\n");
    set_foreground_color(LOG_COLOR_HEADER);
    printf("================================================\n");
    printf(" Type 'help' for available commands\n");
    printf("================================================\n\n");
    set_foreground_color(LOG_COLOR_INFO);

    show_prompt();
    draw_cursor();

    while (1) {
        asm volatile("sti");
        asm volatile("hlt");
        asm volatile("cli");     // Запретить прерывания перед schedule
        schedule();              // Передать управление другим процессам
    }
}

__attribute__((section(".text.prologue")))
void _start(BootInfo* bi) {
    asm volatile ("cli");
    
    initialize_console(bi);
    
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

    // ВАЖНО: сначала PMM, потом PAGING, потом HEAP
    pmm_init(bi->MemoryMap, bi->MemoryMapSize, bi->MemoryMapDescriptorSize,
                bi->KernelBase, bi->KernelSize);
    paging_init(bi);
    
    // Heap теперь статический - инициализируем сразу
    heap_init();  // <-- ВСЯ память выделяется здесь

    // Бутлоадер грузит в RAM ВЕСЬ диск одним куском с LBA 0 (см. подробный
    // комментарий у LUFIRAFS_ESP_SIZE) — первые LUFIRAFS_ESP_SIZE байт это
    // маленький FAT-раздел (ESP) для самой прошивки, а сама LufiraFS
    // начинается сразу за ним. lba_offset нужен lufirafs_sync(), чтобы
    // дописывать "грязные" блоки по правильным абсолютным LBA реального
    // диска через drivers/disk (см. lufirafs.c).
    if (bi->FATImageBase && bi->FATImageSize > LUFIRAFS_ESP_SIZE) {
        LOG_PENDING("Mounting LufiraFS...");
        void *fs_image = (void*)(bi->FATImageBase + LUFIRAFS_ESP_SIZE);
        uint32_t fs_size = (uint32_t)(bi->FATImageSize - LUFIRAFS_ESP_SIZE);
        uint32_t lba_offset = LUFIRAFS_ESP_SIZE / 512;
        if (lufirafs_init(&lufirafs, fs_image, fs_size, lba_offset) == 0) {
            LOG_DONE_OK("LufiraFS mounted");
            devmode_init();
            klog_init();
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

    LOG_PENDING("Initializing syscalls...");
    syscall_init();
    LOG_DONE_OK("Syscalls initialized");

    LOG_PENDING("Initializing VFS...");
    vfs_init();
    LOG_DONE_OK("VFS initialized");

    LOG_PENDING("Initializing keyboard...");
    keyboard_init();
    LOG_DONE_OK("Keyboard %s", keyboard_is_initialized() ? "ready" : "not found");
    
    LOG_PENDING("Initializing mouse...");
    mouse_init();
    LOG_DONE_OK("Mouse %s", mouse_is_initialized() ? "ready" : "not found");

    // pcspeaker_init();

    pci_init();

    if (ac97_init()) {
        printf("[ OK ] AC'97 ready\n");
    } else {
        printf("[WARN] AC'97 unavailable\n");
    }
    
    asm volatile("sti");
    irq_enable(0);  // таймер
    irq_enable(1);  // клавиатура
    irq_enable(2);
    irq_enable(12); // мышь

    // Требует, чтобы прерывания таймера уже тикали (pit_wait_ms() внутри
    // сброса UHCI-контроллера), поэтому вызывается только после sti/irq_enable.
    uhci_init();

    printf("\n");
    set_foreground_color(LOG_COLOR_HEADER);
    printf("================================================\n");
    printf("     LufiraOS Kernel v0.1.0                     \n");
    printf("================================================\n");
    set_foreground_color(LOG_COLOR_INFO);
    
    printf("\n");
    set_foreground_color(LOG_COLOR_HEADER);
    printf("SYSTEM STATUS:\n");
    
    LOG_STATUS_LINE("Console", 1, "READY");
    LOG_STATUS_LINE("Keyboard", keyboard_is_initialized(), keyboard_is_initialized() ? "READY" : "NOT FOUND");
    LOG_STATUS_LINE("Mouse", mouse_is_initialized(), mouse_is_initialized() ? "READY" : "NOT FOUND");
    LOG_STATUS_LINE("Syscalls", 1, "ACTIVE (18 syscalls)");
    LOG_STATUS_LINE("VFS", 1, "READY");
    
    set_foreground_color(STATUS_READY);
    printf("  Memory manager: INITIALIZED\n");
    printf("  Scheduler: COOPERATIVE\n");
    printf("  Process manager: INITIALIZED\n");
    printf("  LufiraFS: %s\n", lufirafs_mounted ? "MOUNTED" : "NOT MOUNTED");
    printf("  Developer mode: %s\n", devmode_is_enabled() ? "ON" : "OFF");
    set_foreground_color(LOG_COLOR_INFO);

    process_set_shell_entry(shell_task);
    process_t *shell_proc = process_create("shell", shell_task);
    if (shell_proc) shell_proc->is_shell = 1;
    
    while (1) {
        asm volatile("sti");
        asm volatile("hlt");
        asm volatile("cli");
        schedule();
    }
}