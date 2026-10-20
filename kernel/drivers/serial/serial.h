#pragma once

#include "lib/types.h"

// Инициализация COM1 (0x3F8) — настраивает UART и определяет через
// loopback-тест, есть ли реально 16550 (см. serial.c). Безопасно звать,
// даже если порта нет — serial_available() вернёт 0.
void serial_init(void);

int serial_available(void);

// Посимвольный блокирующий (опрос LSR, без прерываний) вывод. \n сам по
// себе не досылает \r — это делает вызывающий код (put_char() в console.c).
void serial_putc(char c);
void serial_puts(const char *s);

// Очередной принятый байт, если он уже есть, иначе -1 (не блокирует).
int serial_try_getc(void);
