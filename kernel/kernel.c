#include <stdint.h>
#include <stdarg.h>
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/console.h"
#include "shell/shell.h"
#include "system/gdt.h"
#include "system/idt.h"
#include "fs/fat.h"               // <-- добавлено

// Базовые функции для портов
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void ps2_poll(void) {
    uint8_t status = inb(0x64);

    if (!(status & 1))
        return;                 // данных нет

    if (status & 0x20) {
        mouse_irq_handler();    // байт от мыши
    } else {
        keyboard_irq_handler(); // байт от клавиатуры
    }
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
    gdt_init();
    idt_init();
    pic_remap();                   // маскируем все IRQ от PIC

    // Инициализация FAT, если образ передан
    if (bi->FATImageBase && bi->FATImageSize) {
        if (fat_init(&fatfs, (void*)bi->FATImageBase, bi->FATImageSize) == 0) {
            current_color = convert_color(0x55FF55);
            printf("\n FAT filesystem mounted.\n");
        } else {
            current_color = convert_color(0xFF5555);
            printf("\n FAT mount failed.\n");
        }
    } else {
        current_color = convert_color(0xFF5555);
        printf("\n No FAT image provided.\n");
    }

    display_system_info(bi);
    keyboard_init();
    mouse_init();

    current_color = convert_color(0x55FF55);
    printf("\n Keyboard: READY\n");
    printf(" Mouse: READY\n");
    current_color = convert_color(0x00AAFF);
    printf("\n================================================\n");
    printf(" Type 'help' for available commands\n");
    printf("================================================\n\n");
    current_color = convert_color(0xFFFFFF);

    show_prompt();
    draw_cursor();

    while (1) {
        ps2_poll();
        update_cursor();
        __asm__ volatile ("pause");
    }
}