#pragma once

#include "lib/types.h"

void gdt_init(void);
void gdt_set_tss(uint64_t base, uint32_t limit);

// Селекторы сегментов
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x33  // Ring 3 code (slot 6 | RPL 3)
#define GDT_USER_DATA   0x2B  // Ring 3 data (slot 5 | RPL 3)
#define GDT_TSS         0x18  // TSS selector (slots 3-4)