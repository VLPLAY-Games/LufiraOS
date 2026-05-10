#pragma once

#include <stdint.h>

#if defined(__x86_64__) && defined(__GNUC__)
#define FORCE_ALIGN_ARG_POINTER __attribute__((force_align_arg_pointer))
#else
#define FORCE_ALIGN_ARG_POINTER
#endif

typedef struct __attribute__((packed)) interrupt_frame {
    // Saved general-purpose registers
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rbp;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;

    // Pushed by stubs / CPU
    uint64_t vector;
    uint64_t error_code;

    // CPU exception frame
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
} interrupt_frame_t;

void FORCE_ALIGN_ARG_POINTER isr_common_handler(interrupt_frame_t* frame);