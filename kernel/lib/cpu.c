#include "cpu.h"

int interrupts_enabled(void) {
    uint64_t rflags;
    asm volatile ("pushfq; pop %0" : "=r"(rflags));
    return (int)((rflags >> 9) & 1ULL);
}

// Живой EFLAGS.IF почти бесполезен для команды "status": шелл выполняет
// команды синхронно ИЗНУТРИ keyboard_irq_handler() (см. комментарий про exec
// в elf.c), где CPU аппаратно снимает IF на входе в interrupt gate — то есть
// interrupts_enabled() всегда вернёт 0 в момент выполнения "status", даже
// когда система в целом работает с разрешёнными прерываниями. Отдельный
// флаг фиксирует факт "sti + irq_enable() пройдены на загрузке" один раз.
static int g_interrupts_active = 0;

void cpu_mark_interrupts_active(void) {
    g_interrupts_active = 1;
}

int cpu_interrupts_active(void) {
    return g_interrupts_active;
}