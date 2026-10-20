#pragma once

#include "types.h"

// Проверка флага прерываний (используется в command_status)
int interrupts_enabled(void);

// В отличие от interrupts_enabled() (живой EFLAGS.IF), фиксирует сам факт
// разрешения прерываний на загрузке, а не текущее состояние флага.
void cpu_mark_interrupts_active(void);
int cpu_interrupts_active(void);