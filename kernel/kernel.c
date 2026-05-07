#include <stdint.h>
#include <stdarg.h>
#include "drivers/keyboard.h"
#include "drivers/console.h"
#include "shell/shell.h"
#include "system/gdt.h"
#include "system/idt.h"
#include "system/irq.h"          // <-- добавлено для irq_init()

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

    // Маскируем ВСЕ IRQ – потом irq_init() разрешит нужные
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

// --- Точка входа ядра ---
__attribute__((section(".text.prologue")))
void _start(BootInfo* bi) {
    asm volatile ("cli");

    initialize_console(bi);
    gdt_init();
    idt_init();
    pic_remap();                // перенастройка PIC (все IRQ пока замаскированы)
    irq_init();                 // разрешаем IRQ0 и IRQ1

    display_system_info(bi);
    keyboard_init();

    current_color = convert_color(0x55FF55);
    printf("\n Keyboard: READY\n");
    current_color = convert_color(0x00AAFF);
    printf("\n================================================\n");
    printf(" Type 'help' for available commands\n");
    printf("================================================\n\n");
    current_color = convert_color(0xFFFFFF);

    show_prompt();
    draw_cursor();

    asm volatile ("sti");       // разрешаем маскируемые прерывания

    // Основной idle-цикл – ждём прерывания
    while (1) {
        asm volatile ("hlt");
    }
}