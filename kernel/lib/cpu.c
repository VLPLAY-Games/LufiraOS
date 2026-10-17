#include "cpu.h"

int interrupts_enabled(void) {
    uint64_t rflags;
    asm volatile ("pushfq; pop %0" : "=r"(rflags));
    return (int)((rflags >> 9) & 1ULL);
}

// Живой EFLAGS.IF бесполезен для команды "status": шелл выполняет команды
// синхронно внутри keyboard_irq_handler(), где CPU аппаратно снимает IF —
// interrupts_enabled() там всегда вернёт 0. Этот флаг фиксирует факт, что
// sti + irq_enable() были пройдены на загрузке, один раз.
static int g_interrupts_active = 0;

void cpu_mark_interrupts_active(void) {
    g_interrupts_active = 1;
}

int cpu_interrupts_active(void) {
    return g_interrupts_active;
}