#pragma once

#include "lib/types.h"

void irq_init(void);
void irq_handler(uint64_t vector);     // вызывается из isr_common_handler