#include "tss.h"
#include "gdt.h"
#include "lib/types.h"
#include "lib/stddef.h"

// Выделяем TSS статически (выровнен по 16 байт)
static tss_t tss __attribute__((aligned(16)));

void tss_init(void) {
    // Обнуляем всю структуру
    uint64_t *p = (uint64_t*)&tss;
    for (int i = 0; i < sizeof(tss_t) / 8; i++) {
        p[i] = 0;
    }
    
    // Устанавливаем I/O map base за пределами TSS (означает, что I/O запрещён)
    tss.iomap_base = sizeof(tss_t);
    
    // Устанавливаем дескриптор TSS в GDT
    gdt_set_tss((uint64_t)&tss, sizeof(tss_t));
}

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}

tss_t* tss_get(void) {
    return &tss;
}