#ifndef KERNEL_H
#define KERNEL_H

// Базовые типы для ядра (не конфликтуют с EFI)
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed int i32;

// Структура информации о ядре
typedef struct {
    u64 memory_size;
    u64 kernel_start;
    u64 kernel_end;
} KERNEL_INFO;

// Объявления функций ядра (без типов из EFI)
u64 kernel_main(void);
void kernel_initialize(void);
void kernel_panic(const char *message);

#endif // KERNEL_H