#pragma once

#include "lib/types.h"

// Local APIC + I/O APIC — замена легаси 8259 PIC на реальном железе, где
// 8259 физически не подключён к CPU (прошивка держит платформу в
// APIC-режиме без обходного пути через IMCR назад в PIC-режим — см. разбор
// в kernel.c у pic_force_legacy_mode()). Источник данных — ACPI MADT
// ("APIC" таблица), разбирается здесь же через acpi_find_table_by_signature().
//
// Контракт с остальным ядром не меняется: IRQ по-прежнему адресуются
// "ISA-номером" 0-15 (irq_enable()/irq_disable() в irq.c, SYS_IRQ_REGISTER
// в driver.c), и по-прежнему попадают в CPU строго на вектор 0x20+irq
// (idt_init() эти гейты уже выставляет для IRQ-диапазона 32-47 — менять
// IDT не требуется, меняется только то, ЧТО физически доставляет сигнал
// до CPU: IOAPIC вместо 8259).

// Пытается найти и поднять LAPIC+хотя бы один IOAPIC по ACPI MADT.
// 1 — успех, дальше irq.c использует APIC-путь. 0 — MADT не найдена/не
// распарсилась, LAPIC не включился, или IOAPIC-таблиц нет — вызывающий
// обязан остаться на легаси 8259 (as before this feature existed).
int apic_init(void);

// Истина только после успешного apic_init() — irq.c сверяется с этим на
// каждый enable/disable/EOI, чтобы решить, какой путь использовать.
int apic_available(void);

// Программирует (лениво, при первом enable) redirection entry для данного
// ISA IRQ и снимает маску; irq=0..15. vector — обычно 0x20+irq (см. выше).
// Разруливает ISA IRQ -> GSI через Interrupt Source Override из MADT
// (классический пример — IRQ0 переопределён на GSI2) и берёт
// polarity/trigger mode оттуда же, если override есть, иначе — умолчания
// ISA (active-high, edge-triggered).
void ioapic_enable_irq(uint8_t irq, uint8_t vector);

// Только снимает/ставит маску уже запрограммированной entry (или no-op,
// если эта IRQ ещё не была включена ни разу — нечего маскировать).
void ioapic_disable_irq(uint8_t irq);

// EOI через Local APIC (запись 0 в EOI-регистр) — обязателен ПОСЛЕ каждого
// обслуженного IOAPIC-прерывания, иначе эта линия (и все ниже неё по
// приоритету) больше никогда не доставится. Аналог send_eoi() у 8259, но
// для APIC-пути.
void lapic_eoi(void);
