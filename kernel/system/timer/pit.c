#include "pit.h"
#include "drivers/console/console.h"
#include "system/process/process.h"

// Порты I/O
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Счётчик тиков
static volatile uint64_t pit_ticks = 0;

void pit_init(void) {
    uint16_t divisor = PIT_DIVISOR;
    
    outb(PIT_COMMAND, PIT_SELECT_CH0 | PIT_ACCESS_LOHI | PIT_MODE_SQUARE | PIT_BINARY_MODE);
    outb(PIT_CHANNEL_0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL_0, (uint8_t)((divisor >> 8) & 0xFF));
    
    printf("[PIT] Timer initialized at %u Hz (divisor %u)\n", PIT_FREQUENCY, divisor);
}

void pit_set_frequency(uint32_t hz) {
    if (hz == 0) return;
    
    uint32_t divisor = PIT_BASE_FREQUENCY / hz;
    if (divisor > 65535) divisor = 65535;
    if (divisor < 1) divisor = 1;
    
    outb(PIT_COMMAND, PIT_SELECT_CH0 | PIT_ACCESS_LOHI | PIT_MODE_SQUARE | PIT_BINARY_MODE);
    outb(PIT_CHANNEL_0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL_0, (uint8_t)((divisor >> 8) & 0xFF));
}

uint64_t pit_get_ticks(void) {
    return pit_ticks;
}

// Обработчик прерывания таймера
void timer_irq_handler(void) {
    pit_ticks++;

    process_t *p = process_list;

    if (p) {
        process_t *start = p;

        do {
            if (p->state == PROCESS_SLEEPING &&
                pit_ticks >= p->wakeup_tick)
            {
                p->state = PROCESS_READY;
            }

            p = p->next;
        } while (p && p != start);
    }

    update_cursor();
}