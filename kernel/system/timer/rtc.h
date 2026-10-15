#pragma once

#include "lib/types.h"

typedef struct {
    uint16_t year;   // полный год (4 цифры)
    uint8_t  month;  // 1-12
    uint8_t  day;    // 1-31
    uint8_t  hour;   // 0-23 (уже переведено из 12-часового формата, если он был)
    uint8_t  minute;
    uint8_t  second;
} rtc_time_t;

// Текущее время по CMOS RTC. CMOS RTC самодостаточен без какой-либо
// инициализации с момента включения платы — отдельной rtc_init() не
// нужно, просто читаем. 0 — успех, -1 — после MAX_TRIES попыток
// два последовательных чтения регистров так и не совпали (см. rtc.c) —
// возвращающееся в этом случае *out не трогается.
int rtc_get_time(rtc_time_t *out);
