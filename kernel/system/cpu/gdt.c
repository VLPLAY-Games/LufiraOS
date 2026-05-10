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
    uint16_t limit;
    uint64_t base;
} gdt_ptr_t;

static gdt_entry_t gdt[3] __attribute__((aligned(16)));
static gdt_ptr_t gdt_descriptor;

static void gdt_set_entry(gdt_entry_t* entry, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    entry->limit_low = (uint16_t)(limit & 0xFFFF);
    entry->base_low = (uint16_t)(base & 0xFFFF);
    entry->base_mid = (uint8_t)((base >> 16) & 0xFF);
    entry->access = access;
    entry->granularity = (uint8_t)(((limit >> 16) & 0x0F) | (flags & 0xF0));
    entry->base_high = (uint8_t)((base >> 24) & 0xFF);
}

static void gdt_load(const gdt_ptr_t* gdtr) {
    asm volatile ("lgdt %0" : : "m"(*gdtr) : "memory");

    // Перезагружаем сегментные регистры и CS
    asm volatile (
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        :
        :
        : "rax", "memory", "cc"
    );
}

void gdt_init(void) {
    // 0: null
    gdt_set_entry(&gdt[0], 0, 0, 0, 0);

    // 1: kernel code segment
    // access = 0x9A, flags = 0xA0 (G=1, L=1)
    gdt_set_entry(&gdt[1], 0, 0xFFFFF, 0x9A, 0xA0);

    // 2: kernel data segment
    // access = 0x92, flags = 0xC0 (G=1, D/B=1)
    gdt_set_entry(&gdt[2], 0, 0xFFFFF, 0x92, 0xC0);

    gdt_descriptor.limit = (uint16_t)(sizeof(gdt) - 1);
    gdt_descriptor.base = (uint64_t)&gdt;

    gdt_load(&gdt_descriptor);
}