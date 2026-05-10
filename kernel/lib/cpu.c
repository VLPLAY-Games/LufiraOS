#include "cpu.h"

int interrupts_enabled(void) {
    uint64_t rflags;
    asm volatile ("pushfq; pop %0" : "=r"(rflags));
    return (int)((rflags >> 9) & 1ULL);
}