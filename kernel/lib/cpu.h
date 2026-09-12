#pragma once

#include "types.h"

// Проверка флага прерываний (используется в command_status)
int interrupts_enabled(void);

// Отдельно от живого EFLAGS.IF (см. interrupts_enabled()) — фиксирует факт,
// что прерывания были однократно разрешены на загрузке (kernel.c), а не их
// сиюминутное состояние, которое во время выполнения команды шелла всегда 0.
void cpu_mark_interrupts_active(void);
int cpu_interrupts_active(void);