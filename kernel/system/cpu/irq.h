#pragma once

#include "lib/types.h"
#include "interrupts.h"

void irq_init(void);
// frame — кадр прерывания (см. interrupts.h); нужен timer_irq_handler(),
// чтобы решить, вытеснять ли текущий процесс (см. irq.c).
void irq_handler(uint64_t vector, interrupt_frame_t *frame);   // вызывается из isr_common_handler
void irq_enable(uint8_t irq);
void irq_disable(uint8_t irq);