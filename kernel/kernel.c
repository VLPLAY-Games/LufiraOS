#include "lib/types.h"
#include "lib/stdarg.h"
#include "bootinfo.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "system/mm/heap.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/mouse/mouse.h"
#include "drivers/console/console.h"
#include "shell/shell.h"
#include "system/cpu/gdt.h"
#include "system/cpu/idt.h"
#include "system/cpu/tss.h"
#include "fs/fat/fat.h"
#include "system/cpu/irq.h"
#include "system/acpi/acpi.h"
#include "system/process/process.h"
#include "system/timer/pit.h"
#include "log.h"


static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// PIC
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

fat_fs_t fatfs;

static void test_task_1(void) {
    int counter = 0;
    while (1) {
        set_foreground_color(COLOR_LIGHT_GREEN);
        printf("[Task 1] Counter: %d, Ticks: %u\n", counter, (uint32_t)pit_get_ticks());
        set_foreground_color(COLOR_WHITE);
        counter++;
        if (counter >= 10) {
            printf("[Task 1] Done, exiting\n");
            process_exit();
        }
    }
}

static void test_task_2(void) {
    int counter = 0;
    while (1) {
        set_foreground_color(COLOR_LIGHT_CYAN);
        printf("[Task 2] Counter: %d, Ticks: %u\n", counter, (uint32_t)pit_get_ticks());
        set_foreground_color(COLOR_WHITE);
        counter++;
        if (counter >= 10) {
            printf("[Task 2] Done, exiting\n");
            process_exit();
        }
    }
}

static void test_task_3(void) {
    uint64_t last_ticks = 0;
    while (1) {
        uint64_t current_ticks = pit_get_ticks();
        if (current_ticks - last_ticks >= 50) {
            set_foreground_color(COLOR_YELLOW);
            printf("[Heart] Beat at tick %u\n", (uint32_t)current_ticks);
            set_foreground_color(COLOR_WHITE);
            last_ticks = current_ticks;
        }
    }
}

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
    }
}

// --- Точка входа ядра ---
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

    pmm_init(bi->MemoryMap, bi->MemoryMapSize, bi->MemoryMapDescriptorSize,
                bi->KernelBase, bi->KernelSize);
    paging_init(bi);
    heap_init();

    if (bi->FATImageBase && bi->FATImageSize) {
        LOG_PENDING("Mounting FAT filesystem...");
        if (fat_init(&fatfs, (void*)bi->FATImageBase, bi->FATImageSize) == 0) {
            LOG_DONE_OK("FAT filesystem mounted");
        } else {
            LOG_DONE_FAIL("FAT mount failed");
        }
    } else {
        LOG_FAIL("No FAT image provided");
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

    LOG_INFO_LINE("");
    LOG_INFO_LINE("Boot Information");
    LOG_INFO_LINE("Kernel base: 0x%lx", bi->KernelBase);
    LOG_INFO_LINE("Kernel size: %u KB", (uint32_t)(bi->KernelSize / 1024));
    LOG_INFO_LINE("Total memory: %u MB", (uint32_t)(bi->TotalMemory / (1024 * 1024)));

    LOG_INFO_LINE("");
    LOG_INFO_LINE("Display Information");
    LOG_INFO_LINE("Framebuffer: 0x%lx", bi->FrameBufferBase);
    LOG_INFO_LINE("Resolution: %dx%d", bi->HorizontalResolution, bi->VerticalResolution);

    LOG_PENDING("Initializing keyboard...");
    keyboard_init();
    LOG_DONE_OK("Keyboard %s", keyboard_is_initialized() ? "ready" : "not found");
    
    LOG_PENDING("Initializing mouse...");
    mouse_init();
    LOG_DONE_OK("Mouse %s", mouse_is_initialized() ? "ready" : "not found");
    
    LOG_PENDING("Enabling interrupts...");
    irq_init();
    LOG_DONE_OK("Interrupts enabled");
    
    asm volatile("sti");

    printf("\n");
    set_foreground_color(LOG_COLOR_HEADER);
    printf("================================================\n");
    printf("     LufiraOS Kernel v0.4                        \n");
    printf("================================================\n");
    set_foreground_color(LOG_COLOR_INFO);
    
    printf("\n");
    set_foreground_color(LOG_COLOR_HEADER);
    printf("SYSTEM STATUS:\n");
    
    LOG_STATUS_LINE("Console", 1, "READY (256 colors)");
    LOG_STATUS_LINE("Keyboard", keyboard_is_initialized(), 
        keyboard_is_initialized() ? "READY" : "NOT DETECTED");
    LOG_STATUS_LINE("Mouse", mouse_is_initialized(),
        mouse_is_initialized() ? "READY" : "NOT DETECTED");
    
    set_foreground_color(STATUS_READY);
    printf("  Memory manager: INITIALIZED\n");
    printf("  Scheduler: PREEMPTIVE (%d Hz)\n", PIT_FREQUENCY);
    printf("  Process manager: INITIALIZED\n");
    set_foreground_color(LOG_COLOR_INFO);

    printf("\n");
    set_foreground_color(LOG_COLOR_HEADER);
    printf("STARTING PROCESSES:\n");
    set_foreground_color(LOG_COLOR_INFO);
    
    process_create("test_task_1", test_task_1);
    process_create("test_task_2", test_task_2);
    process_create("heartbeat", test_task_3);
    process_create("shell", shell_task);
    
    printf("\nAll processes created! Preemptive scheduling active.\n");
    
    schedule();
    
    while (1) __asm__("hlt");
}