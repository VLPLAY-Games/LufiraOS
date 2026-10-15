#pragma once

#include "lib/types.h"

// Находит HPET через ACPI (таблица "HPET"), мапит его MMIO-регион
// (mmio_map(), см. paging.h) и включает свободно бегущий главный счётчик.
// Сравнивающие таймеры/их прерывания НЕ используются вовсе — нужен только
// точный источник времени (polled), тот же принцип, что у остального
// ввода-вывода в этом ядре. Безопасно звать, даже если HPET на платформе
// нет вообще (QEMU с -no-hpet, старое реальное железо, ...) —
// hpet_available() после этого просто вернёт 0.
void hpet_init(void);

int hpet_available(void);

// Сырое значение главного счётчика (тиков с момента hpet_init()).
uint64_t hpet_read_counter(void);

uint64_t hpet_ticks_per_second(void);

// Главный счётчик, переведённый в наносекунды с момента hpet_init().
uint64_t hpet_read_ns(void);

// Busy-wait на основе HPET — точнее pit_wait_ms() для коротких
// (микросекундных) задержек, которые PIT (100 Гц, см. pit.h) просто не
// умеет мерить. No-op, если HPET недоступен.
void hpet_wait_us(uint32_t us);
