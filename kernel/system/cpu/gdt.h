#pragma once

#include "lib/types.h"

void gdt_init(void);
void gdt_set_tss(uint64_t base, uint32_t limit);

// Селекторы сегментов
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x1B  // Ring 3 code (0x18 | RPL 3)
#define GDT_USER_DATA   0x23  // Ring 3 data (0x20 | RPL 3)
#define GDT_TSS         0x18  // TSS selector