#include "kernel.h"

// Простое 64-битное ядро
UINT64 kernel_main(void)
{
    // Точка входа в ядро
    // Пока просто возвращаем значение
    return 0xDEADBEEFCAFEBABE;
}

// Функции ядра будут добавляться здесь
void kernel_initialize(void)
{
    // Инициализация ядра
}

void kernel_panic(const char *message)
{
    // Паника ядра
    while(1) __asm__("hlt");
}