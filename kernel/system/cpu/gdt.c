#include "gdt.h"
#include "lib/types.h"

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} gdt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} tss_descriptor_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdt_ptr_t;

// 8 записей: null, kernel code, kernel data, 
//           user code (2 слота), user data (2 слота), TSS
static uint64_t gdt[8] __attribute__((aligned(16)));
static gdt_ptr_t gdt_descriptor;

static void gdt_set_entry(int index, uint32_t base, uint32_t limit, 
                          uint8_t access, uint8_t flags) {
    gdt_entry_t *entry = (gdt_entry_t*)&gdt[index];
    entry->limit_low = (uint16_t)(limit & 0xFFFF);
    entry->base_low = (uint16_t)(base & 0xFFFF);
    entry->base_mid = (uint8_t)((base >> 16) & 0xFF);
    entry->access = access;
    entry->granularity = (uint8_t)(((limit >> 16) & 0x0F) | (flags & 0xF0));
    entry->base_high = (uint8_t)((base >> 24) & 0xFF);
}

void gdt_set_tss(uint64_t base, uint32_t limit) {
    tss_descriptor_t *tss_desc = (tss_descriptor_t*)&gdt[3];
    
    tss_desc->limit_low = limit & 0xFFFF;
    tss_desc->base_low = base & 0xFFFF;
    tss_desc->base_mid = (base >> 16) & 0xFF;
    tss_desc->access = 0x89;      // Present, 64-bit TSS (available)
    tss_desc->granularity = 0x00;
    tss_desc->base_high = (base >> 24) & 0xFF;
    tss_desc->base_upper = (base >> 32) & 0xFFFFFFFF;
    tss_desc->reserved = 0;
    
    // Загружаем TSS
    asm volatile (
        "movw %0, %%ax\n"
        "ltr %%ax\n"
        :
        : "i"(GDT_TSS)
        : "ax", "memory"
    );
}

static void gdt_load(const gdt_ptr_t* gdtr) {
    asm volatile ("lgdt %0" : : "m"(*gdtr) : "memory");

    // Перезагружаем сегментные регистры
    asm volatile (
        "movw %0, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "pushq %1\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        :
        : "i"(GDT_KERNEL_DATA), "i"(GDT_KERNEL_CODE)
        : "rax", "memory", "cc"
    );
}

void gdt_init(void) {
    for (int i = 0; i < 8; i++) gdt[i] = 0;
    
    // 0: null
    gdt_set_entry(0, 0, 0, 0, 0);

    // 1: kernel code (0x08) - access=0x9A (present, ring 0, code)
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0);

    // 2: kernel data (0x10) - access=0x92 (present, ring 0, data, writable)
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);
    
    // 3-4: TSS (будет установлен позже)
    
    // 5: user data (0x20) - access=0xF2 (present, ring 3, data, writable)
    gdt_set_entry(5, 0, 0xFFFFF, 0xF2, 0xC0);
    
    // 6: user code (0x28) - access=0xFA (present, ring 3, code)
    gdt_set_entry(6, 0, 0xFFFFF, 0xFA, 0xA0);

    gdt_descriptor.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_descriptor.base = (uint64_t)&gdt;

    gdt_load(&gdt_descriptor);
}