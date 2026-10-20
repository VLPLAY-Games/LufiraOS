// serial.c — UART 16550 (COM1, 0x3F8), опросный (polled), без прерываний.
// Назначение: отладочный вывод, не зависящий от графической консоли —
// put_char() (console.c) зеркалит туда каждый символ, так что
// `qemu ... -serial stdio` показывает вывод даже без видео.
#include "serial.h"

#define COM1_PORT 0x3F8

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

#define REG_DATA        0 // RBR/THR (DLAB=0) или делитель, младший байт (DLAB=1)
#define REG_IER         1 // Interrupt Enable (DLAB=0) или делитель, старший байт (DLAB=1)
#define REG_FCR         2 // FIFO Control (запись)
#define REG_LCR         3 // Line Control (бит 7 — DLAB)
#define REG_MCR         4 // Modem Control
#define REG_LSR         5 // Line Status

#define LCR_8N1         0x03
#define LCR_DLAB        0x80
#define FCR_ENABLE_FIFO 0xC7 // включить FIFO, сбросить RX/TX, порог 14 байт
#define MCR_LOOPBACK    0x1E // DTR+RTS+OUT1+OUT2 и бит loopback
#define MCR_NORMAL      0x0F // DTR+RTS+OUT1+OUT2, loopback снят
#define LSR_DATA_READY  0x01
#define LSR_THR_EMPTY   0x20

static int g_serial_available = 0;

void serial_init(void) {
    outb(COM1_PORT + REG_IER, 0x00); // прерывания не используем — polled

    // Делитель скорости: 115200 / baud. Делитель 1 = 115200 бод — дефолт
    // для QEMU `-serial` без явного указания скорости.
    outb(COM1_PORT + REG_LCR, LCR_DLAB);
    outb(COM1_PORT + REG_DATA, 1);    // делитель, младший байт
    outb(COM1_PORT + REG_IER, 0);     // делитель, старший байт
    outb(COM1_PORT + REG_LCR, LCR_8N1); // 8 бит, без паритета, 1 стоп-бит; DLAB снят

    outb(COM1_PORT + REG_FCR, FCR_ENABLE_FIFO);

    // Loopback-тест (стандартный приём определения живого 16550): шлём
    // контрольный байт сами себе и проверяем, что он же пришёл. Без порта
    // LSR никогда не покажет "THR empty", и serial_putc() завис бы в
    // ожидании — поэтому проверяем заранее.
    outb(COM1_PORT + REG_MCR, MCR_LOOPBACK);
    outb(COM1_PORT + REG_DATA, 0xAE);
    g_serial_available = (inb(COM1_PORT + REG_DATA) == 0xAE);

    outb(COM1_PORT + REG_MCR, MCR_NORMAL);
}

int serial_available(void) {
    return g_serial_available;
}

void serial_putc(char c) {
    if (!g_serial_available) return;

    // Ограниченное ожидание вместо `while(1)` — порт уже подтверждён живым
    // в serial_init(), но так надёжнее.
    for (int tries = 0; tries < 100000; tries++) {
        if (inb(COM1_PORT + REG_LSR) & LSR_THR_EMPTY) {
            outb(COM1_PORT + REG_DATA, (uint8_t)c);
            return;
        }
    }
}

void serial_puts(const char *s) {
    if (!s) return;
    while (*s) serial_putc(*s++);
}

int serial_try_getc(void) {
    if (!g_serial_available) return -1;
    if (!(inb(COM1_PORT + REG_LSR) & LSR_DATA_READY)) return -1;
    return (int)inb(COM1_PORT + REG_DATA);
}
