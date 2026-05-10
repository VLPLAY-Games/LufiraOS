#pragma once

#include "lib/types.h"

// Частота PIT (базовая)
#define PIT_BASE_FREQUENCY 1193180

// Порты PIT
#define PIT_CHANNEL_0   0x40
#define PIT_CHANNEL_1   0x41
#define PIT_CHANNEL_2   0x42
#define PIT_COMMAND     0x43

// Команды PIT
#define PIT_SELECT_CH0  0x00    // Выбрать канал 0
#define PIT_ACCESS_LOHI 0x30    // Доступ: сначала младший, потом старший байт
#define PIT_MODE_SQUARE 0x06    // Режим 3: square wave generator
#define PIT_BINARY_MODE 0x00    // 16-битный бинарный режим

// Частота планировщика (100 Гц = 10 мс квант)
#define PIT_FREQUENCY   100
#define PIT_DIVISOR     (PIT_BASE_FREQUENCY / PIT_FREQUENCY)

// Функции
void pit_init(void);
void pit_set_frequency(uint32_t hz);
uint64_t pit_get_ticks(void);