#include "irq.h"
#include "drivers/console/console.h"
#include "drivers/keyboard/keyboard.h"
#include "drivers/mouse/mouse.h"
#include "system/timer/pit.h"
#include "system/driver/driver.h"
#include "system/cpu/apic.h"

// Прямая работа с портами PIC — путь по умолчанию, ЕСЛИ apic_available()
// не говорит обратного (см. apic.c/apic.h: на части реального железа 8259
// физически не доходит до CPU вообще никак, там единственный рабочий путь
// — LAPIC+IOAPIC; на остальном/под QEMU оба пути эквивалентны, но PIC —
// то, что уже было проверено временем, поэтому остаётся умолчанием).
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
    if (apic_available()) {
        lapic_eoi();
        return;
    }
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

// Основной обработчик аппаратных прерываний.
//
// EOI шлётся СРАЗУ, до диспетчеризации в конкретный обработчик: обработчики
// (вытеснение в timer_irq_handler(), run/exec-команда шелла изнутри IRQ)
// иногда уходят в switch_to_process()/context_switch()/context_enter_ring3()
// и МОГУТ НЕ ВЕРНУТЬСЯ сюда (или вообще, если это exec) — "EOI в конце
// функции" тогда не выполнился бы, и IRQ навсегда считался бы "в
// обслуживании". Это безопасно и для обычно возвращающихся обработчиков:
// все ISR-заглушки делают cli на входе (interrupts.S), IF нигде не
// взводится обратно внутри обработчиков, так что более ранний EOI не
// порождает реальное вложенное прерывание — лишь сдвигает момент, когда PIC
// готов его доставить, а не когда CPU реально его примет.
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
            // Любая линия без зашитого в ядро обработчика — возможно,
            // закреплена за userspace-драйвером (driver.h/SYS_IRQ_REGISTER).
            // Не делает ничего, если никто её не регистрировал.
            driver_irq_notify(irq);
            break;
    }
}

// Включение конкретной линии IRQ
void irq_enable(uint8_t irq) {
    if (apic_available()) {
        // IRQ2 — легаси cascade (master<-slave 8259), у IOAPIC нет такого
        // понятия: каждый GSI независим. Более того, GSI2 почти всегда УЖЕ
        // занят Interrupt Source Override для ИСТИННОГО IRQ0 (классическая
        // ISA-плата подключала таймер именно туда) — попытка здесь
        // запрограммировать GSI2 под "IRQ2" затёрла бы редирект таймера.
        // Поэтому IRQ2 на APIC-пути — осознанный no-op.
        if (irq == 2) return;
        ioapic_enable_irq(irq, (uint8_t)(0x20 + irq));
        return;
    }

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
    if (apic_available()) {
        if (irq == 2) return; // см. irq_enable() — IRQ2 тут не существует как отдельная линия
        ioapic_disable_irq(irq);
        return;
    }

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