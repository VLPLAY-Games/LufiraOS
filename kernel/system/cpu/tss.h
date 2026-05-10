#pragma once

#include "lib/types.h"

// Структура TSS для 64-битного режима (long mode)
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;      // Stack pointer for ring 0
    uint64_t rsp1;      // Stack pointer for ring 1
    uint64_t rsp2;      // Stack pointer for ring 2
    uint64_t reserved1;
    uint64_t ist1;      // Interrupt stack table 1
    uint64_t ist2;      // Interrupt stack table 2
    uint64_t ist3;      // Interrupt stack table 3
    uint64_t ist4;      // Interrupt stack table 4
    uint64_t ist5;      // Interrupt stack table 5
    uint64_t ist6;      // Interrupt stack table 6
    uint64_t ist7;      // Interrupt stack table 7
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base; // I/O map base address
} tss_t;

void tss_init(void);
void tss_set_rsp0(uint64_t rsp0);
tss_t* tss_get(void);