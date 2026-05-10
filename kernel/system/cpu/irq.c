#include "irq.h"
#include "../../drivers/console/console.h"
#include "../../drivers/keyboard/keyboard.h"
#include "../../drivers/mouse/mouse.h"

// Прямая работа с портами PIC
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

static void send_eoi(uint8_t irq) {
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

// Основной обработчик аппаратных прерываний
void irq_handler(uint64_t vector) {
    uint8_t irq = (uint8_t)(vector - 0x20);

    switch (irq) {
        case 0:                     // системный таймер
            update_cursor();        // мигание курсора
            break;
        case 1:                     // клавиатура
            keyboard_irq_handler();
            break;
        case 12:                    // мышь PS/2
            mouse_irq_handler();
            break;
        default:
            break;
    }

    send_eoi(irq);
}

// Включение конкретной линии IRQ
void irq_enable(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    value = inb(port) & ~(1 << irq);
    outb(port, value);
}

// Выключение линии IRQ
void irq_disable(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    value = inb(port) | (1 << irq);
    outb(port, value);
}

// Инициализация: разрешаем IRQ0 и IRQ1
void irq_init(void) {
    irq_enable(0);   // таймер
    irq_enable(1);   // клавиатура
    irq_enable(12);   // mouse
}