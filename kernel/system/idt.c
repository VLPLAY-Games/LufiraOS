#include "idt.h"
#include "interrupts.h"
#include "drivers/console.h"

#include <stdint.h>
#include <stddef.h>

extern void (*isr_stub_table[])(void);

typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idt_ptr_t;

static idt_entry_t idt[256] __attribute__((aligned(16)));
static idt_ptr_t idt_descriptor;

static const char* const exception_names[32] = {
    "Divide by zero",               // 0
    "Debug",                        // 1
    "NMI",                          // 2
    "Breakpoint",                   // 3
    "Overflow",                     // 4
    "Bound range exceeded",         // 5
    "Invalid opcode",               // 6
    "Device not available",         // 7
    "Double fault",                 // 8
    "Coprocessor segment overrun",   // 9
    "Invalid TSS",                  // 10
    "Segment not present",          // 11
    "Stack-segment fault",          // 12
    "General protection fault",     // 13
    "Page fault",                   // 14
    "Reserved",                     // 15
    "x87 floating-point exception", // 16
    "Alignment check",              // 17
    "Machine check",                // 18
    "SIMD floating-point exception",// 19
    "Virtualization exception",     // 20
    "Control protection exception", // 21
    "Reserved",                     // 22
    "Reserved",                     // 23
    "Reserved",                     // 24
    "Reserved",                     // 25
    "Reserved",                     // 26
    "Reserved",                     // 27
    "Hypervisor injection",         // 28
    "VMM communication",            // 29
    "Security exception",           // 30
    "Reserved"                      // 31
};

static void idt_set_gate(uint8_t vector, uintptr_t handler, uint16_t selector, uint8_t type_attr) {
    idt[vector].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[vector].selector    = selector;
    idt[vector].ist         = 0;
    idt[vector].type_attr   = type_attr;
    idt[vector].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[vector].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFF);
    idt[vector].zero        = 0;
}

static void idt_load(const idt_ptr_t* idtr) {
    asm volatile ("lidt %0" : : "m"(*idtr) : "memory");
}

static void hex64_to_string(uint64_t value, char out[19]) {
    static const char digits[] = "0123456789ABCDEF";

    out[0] = '0';
    out[1] = 'x';

    for (int i = 0; i < 16; ++i) {
        out[2 + i] = digits[(value >> ((15 - i) * 4)) & 0xF];
    }

    out[18] = '\0';
}

static void print_hex_value(uint64_t value) {
    char buf[19];
    hex64_to_string(value, buf);
    printf(buf);
}

static void print_hex_line(const char* label, uint64_t value) {
    printf("  ");
    printf(label);
    printf(": ");
    print_hex_value(value);
    printf("\n");
}

static const char* exception_name(uint64_t vector) {
    if (vector < 32) {
        return exception_names[vector];
    }
    return "Unknown exception";
}

static uint64_t read_cr2(void) {
    uint64_t value;
    asm volatile ("mov %%cr2, %0" : "=r"(value));
    return value;
}

static void print_page_fault_details(uint64_t error_code) {
    printf("  PF flags: ");
    printf("P=");
    printf((error_code & 1) ? "1" : "0");
    printf(" W/R=");
    printf((error_code & 2) ? "1" : "0");
    printf(" U/S=");
    printf((error_code & 4) ? "1" : "0");
    printf(" RSVD=");
    printf((error_code & 8) ? "1" : "0");
    printf(" I/D=");
    printf((error_code & 16) ? "1" : "0");
    printf("\n");

    print_hex_line("CR2", read_cr2());
}

void FORCE_ALIGN_ARG_POINTER isr_common_handler(interrupt_frame_t* frame) {
    current_color = convert_color(0xFF5555);

    printf("\n\n================ EXCEPTION ================\n");
    printf("  Vector      : ");
    print_hex_value(frame->vector);
    printf(" (");
    printf(exception_name(frame->vector));
    printf(")\n");

    print_hex_line("Error code", frame->error_code);
    print_hex_line("RIP", frame->rip);
    print_hex_line("CS", frame->cs);
    print_hex_line("RFLAGS", frame->rflags);

    if (frame->vector == 14) {
        print_page_fault_details(frame->error_code);
    } else if (frame->vector == 13 && frame->error_code != 0) {
        print_hex_line("GPF selector index", (frame->error_code >> 3));
    }

    printf("\n  Register dump:\n");
    print_hex_line("RAX", frame->rax);
    print_hex_line("RBX", frame->rbx);
    print_hex_line("RCX", frame->rcx);
    print_hex_line("RDX", frame->rdx);
    print_hex_line("RBP", frame->rbp);
    print_hex_line("RSI", frame->rsi);
    print_hex_line("RDI", frame->rdi);
    print_hex_line("R8",  frame->r8);
    print_hex_line("R9",  frame->r9);
    print_hex_line("R10", frame->r10);
    print_hex_line("R11", frame->r11);
    print_hex_line("R12", frame->r12);
    print_hex_line("R13", frame->r13);
    print_hex_line("R14", frame->r14);
    print_hex_line("R15", frame->r15);

    printf("\n  System halted.\n");
    current_color = convert_color(0xFFFFFF);

    for (;;) {
        asm volatile ("cli; hlt");
    }
}

void idt_init(void) {
    // Vectors 0..31 are CPU exceptions
    idt_set_gate(0,  (uintptr_t)isr_stub_table[0],  0x08, 0x8E);
    idt_set_gate(1,  (uintptr_t)isr_stub_table[1],  0x08, 0x8E);
    idt_set_gate(2,  (uintptr_t)isr_stub_table[2],  0x08, 0x8E);
    idt_set_gate(3,  (uintptr_t)isr_stub_table[3],  0x08, 0x8E);
    idt_set_gate(4,  (uintptr_t)isr_stub_table[4],  0x08, 0x8E);
    idt_set_gate(5,  (uintptr_t)isr_stub_table[5],  0x08, 0x8E);
    idt_set_gate(6,  (uintptr_t)isr_stub_table[6],  0x08, 0x8E);
    idt_set_gate(7,  (uintptr_t)isr_stub_table[7],  0x08, 0x8E);
    idt_set_gate(8,  (uintptr_t)isr_stub_table[8],  0x08, 0x8E);
    idt_set_gate(9,  (uintptr_t)isr_stub_table[9],  0x08, 0x8E);
    idt_set_gate(10, (uintptr_t)isr_stub_table[10], 0x08, 0x8E);
    idt_set_gate(11, (uintptr_t)isr_stub_table[11], 0x08, 0x8E);
    idt_set_gate(12, (uintptr_t)isr_stub_table[12], 0x08, 0x8E);
    idt_set_gate(13, (uintptr_t)isr_stub_table[13], 0x08, 0x8E);
    idt_set_gate(14, (uintptr_t)isr_stub_table[14], 0x08, 0x8E);
    idt_set_gate(15, (uintptr_t)isr_stub_table[15], 0x08, 0x8E);
    idt_set_gate(16, (uintptr_t)isr_stub_table[16], 0x08, 0x8E);
    idt_set_gate(17, (uintptr_t)isr_stub_table[17], 0x08, 0x8E);
    idt_set_gate(18, (uintptr_t)isr_stub_table[18], 0x08, 0x8E);
    idt_set_gate(19, (uintptr_t)isr_stub_table[19], 0x08, 0x8E);
    idt_set_gate(20, (uintptr_t)isr_stub_table[20], 0x08, 0x8E);
    idt_set_gate(21, (uintptr_t)isr_stub_table[21], 0x08, 0x8E);
    idt_set_gate(22, (uintptr_t)isr_stub_table[22], 0x08, 0x8E);
    idt_set_gate(23, (uintptr_t)isr_stub_table[23], 0x08, 0x8E);
    idt_set_gate(24, (uintptr_t)isr_stub_table[24], 0x08, 0x8E);
    idt_set_gate(25, (uintptr_t)isr_stub_table[25], 0x08, 0x8E);
    idt_set_gate(26, (uintptr_t)isr_stub_table[26], 0x08, 0x8E);
    idt_set_gate(27, (uintptr_t)isr_stub_table[27], 0x08, 0x8E);
    idt_set_gate(28, (uintptr_t)isr_stub_table[28], 0x08, 0x8E);
    idt_set_gate(29, (uintptr_t)isr_stub_table[29], 0x08, 0x8E);
    idt_set_gate(30, (uintptr_t)isr_stub_table[30], 0x08, 0x8E);
    idt_set_gate(31, (uintptr_t)isr_stub_table[31], 0x08, 0x8E);

    idt_descriptor.limit = (uint16_t)(sizeof(idt) - 1);
    idt_descriptor.base = (uint64_t)&idt;

    idt_load(&idt_descriptor);
}