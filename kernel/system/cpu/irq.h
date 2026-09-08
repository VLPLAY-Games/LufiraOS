#pragma once

#include "lib/types.h"

void irq_init(void);
void irq_handler(uint64_t vector);     // вызывается из isr_common_handler
void irq_enable(uint8_t irq);
void irq_disable(uint8_t irq);