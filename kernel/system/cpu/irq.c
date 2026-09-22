#include "irq.h"
#include "drivers/console/console.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/mouse/mouse.h"
#include "system/timer/pit.h"

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

// Основной обработчик аппаратных прерываний.
//
// EOI шлётся СРАЗУ, до диспетчеризации в конкретный обработчик — не после,
// как было раньше. Причина: обработчики (timer_irq_handler() при
// вытеснении, run/exec-команда шелла, вызванная синхронно изнутри IRQ)
// иногда уходят в switch_to_process()/context_switch()/context_enter_ring3()
// и МОГУТ НЕ ВЕРНУТЬСЯ сюда ещё очень долго (или вообще, если это был
// exec) — тогда "EOI в конце функции" просто никогда не выполнился бы, и
// соответствующий IRQ навсегда считался бы "в обслуживании" (PIC перестал
// бы его доставлять). Раньше это обходили точечными ручными EOI в elf.c
// перед каждым таким прыжком — теперь один источник истины здесь. Это
// безопасно даже для обработчиков, которые ВОЗВРАЩАЮТСЯ нормально: все
// ISR-заглушки делают cli на входе (interrupts.S), и внутри обработчиков
// IF нигде не взводится обратно, так что более ранний EOI не может вызвать
// реальное вложенное прерывание — он лишь meняет момент, когда PIC был бы
// ГОТОВ его доставить, а не когда CPU реально его примет.
void irq_handler(uint64_t vector, interrupt_frame_t *frame) {
    uint8_t irq = (uint8_t)(vector - 0x20);

    send_eoi(irq);

    switch (irq) {
        case 0:                     // системный таймер
            timer_irq_handler(frame);
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
    irq_enable(12);  // mouse
}