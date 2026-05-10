#pragma once

#include "lib/types.h"

void gdt_init(void);
void gdt_set_tss(uint64_t base, uint32_t limit);