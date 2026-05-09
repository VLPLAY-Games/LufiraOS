#include <stdint.h>
#include <stdarg.h>
#include "system/pmm.h"
#include "system/paging.h"
#include "system/heap.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/console.h"
#include "shell/shell.h"
#include "system/gdt.h"
#include "system/idt.h"
#include "fs/fat.h"
#include "system/irq.h"
#include "system/log.h"


// Базовые функции для портов
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
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    outb(PIC1_DATA, 0x20);    // master offset
    outb(PIC2_DATA, 0x28);    // slave offset
    outb(PIC1_DATA, 0x04);    // tell master about slave at IRQ2
    outb(PIC2_DATA, 0x02);    // tell slave its cascade identity
    outb(PIC1_DATA, 0x01);    // ICW4
    outb(PIC2_DATA, 0x01);    // ICW4
    outb(PIC1_DATA, 0xFF);    // mask all IRQs on master
    outb(PIC2_DATA, 0xFF);    // mask all IRQs on slave
}

fat_fs_t fatfs;   // глобальная файловая система (для shell)

// --- Точка входа ядра ---
__attribute__((section(".text.prologue")))
void _start(BootInfo* bi) {
    asm volatile ("cli");

    initialize_console(bi);
    
    // ========== РАННЯЯ ИНИЦИАЛИЗАЦИЯ ==========
    LOG_PENDING("Initializing GDT...");
    gdt_init();
    LOG_DONE_OK("GDT initialized");
    
    LOG_PENDING("Initializing IDT...");
    idt_init();
    LOG_DONE_OK("IDT initialized");
    
    LOG_PENDING("Remapping PIC...");
    pic_remap();
    LOG_DONE_OK("PIC remapped");

    // ========== МЕНЕДЖЕРЫ ПАМЯТИ (со своим выводом) ==========
    pmm_init(bi->MemoryMap, bi->MemoryMapSize, bi->MemoryMapDescriptorSize,
                bi->KernelBase, bi->KernelSize);
    paging_init(bi);
    heap_init();

    // ========== ФАЙЛОВАЯ СИСТЕМА ==========
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

    // ========== ИНФОРМАЦИЯ О ЗАГРУЗКЕ ==========
    LOG_SECTION("Boot Information");
    LOG_KV("Kernel base", "0x%lx", bi->KernelBase);
    LOG_KV("Kernel size", "%u KB", (uint32_t)(bi->KernelSize / 1024));
    LOG_KV("Total memory", "%u MB", (uint32_t)(bi->TotalMemory / (1024 * 1024)));
    LOG_KV("Memory map size", "%u bytes", (uint32_t)bi->MemoryMapSize);
    LOG_KV("Descriptor size", "%u bytes", bi->MemoryMapDescriptorSize);
    if (bi->RsdpAddress)
        LOG_KV("RSDP", "0x%lx", bi->RsdpAddress);
    if (bi->SmbiosAddress)
        LOG_KV("SMBIOS", "0x%lx", bi->SmbiosAddress);

    // ========== ИНФОРМАЦИЯ О ДИСПЛЕЕ ==========
    LOG_SECTION("Display Information");
    LOG_KV("Resolution", "%d x %d", bi->HorizontalResolution, bi->VerticalResolution);
    LOG_KV("Pixel format", "%s", (bi->PixelFormat == 0) ? "RGB" : "BGR");
    LOG_KV("Pixels per line", "%d", bi->PixelsPerScanLine);
    LOG_KV("Console grid", "%d x %d chars", screen_width_chars, screen_height_chars);

    // ========== ЗАГОЛОВОК ЯДРА ==========
    printf("\n");
    set_foreground_color(LOG_COLOR_HEADER);
    printf("================================================\n");
    printf("             LufiraOS Kernel v0.2               \n");
    printf("================================================\n");
    set_foreground_color(LOG_COLOR_INFO);
    
    LOG_SECTION("System Information");
    LOG_KV("Architecture", "x86_64");
    LOG_KV("Build date", "%s", __DATE__);
    LOG_KV("Build time", "%s", __TIME__);

    // ========== ИНИЦИАЛИЗАЦИЯ УСТРОЙСТВ ==========
    LOG_PENDING("Initializing keyboard...");
    keyboard_init();
    if (keyboard_is_initialized()) {
        LOG_DONE_OK("Keyboard initialized");
    } else {
        LOG_DONE_FAIL("Keyboard not detected");
    }
    
    LOG_PENDING("Initializing mouse...");
    mouse_init();
    if (mouse_is_initialized()) {
        LOG_DONE_OK("Mouse initialized");
    } else {
        LOG_DONE_WARN("Mouse not detected");
    }
    
    LOG_PENDING("Enabling interrupts...");
    irq_init();
    LOG_DONE_OK("Interrupts enabled");
    
    asm volatile("sti");

    // ========== СТАТУС СИСТЕМЫ ==========
    LOG_SECTION("System Status");
    LOG_KV("Console", "READY (256 colors)");
    LOG_KV("Keyboard", "%s", keyboard_is_initialized() ? "READY" : "NOT DETECTED");
    LOG_KV("Mouse", "%s", mouse_is_initialized() ? "READY" : "NOT DETECTED");
    LOG_KV("Memory manager", "INITIALIZED");
    LOG_KV("Interrupts", "ENABLED");
    LOG_KV("Heap", "0x%lx", KERNEL_HEAP_START);

    // ========== ПРИГЛАШЕНИЕ ==========
    set_foreground_color(LOG_COLOR_HEADER);
    printf("\n================================================\n");
    printf(" Type 'help' for available commands\n");
    printf("================================================\n\n");
    set_foreground_color(LOG_COLOR_INFO);

    show_prompt();
    draw_cursor();

    while (1) {
        asm volatile("hlt");
    }
}