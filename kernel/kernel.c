/* kernel.c - основное ядро LufiraOS */

#include <stdint.h>
#include "keyboard.h"
#include "shell.h"

/* Глобальные переменные курсора (объявлены в shell.h) */
uint32_t cursor_x = 0;
uint32_t cursor_y = 0;

/* Главная функция ядра */
void kernel_main(void) {
    /* Инициализация терминала */
    terminal_init();

    /* Приветственное сообщение */
    terminal_setcolor(make_color(COLOR_GREEN, COLOR_BLACK));
    terminal_writestring("Welcome to LufiraOS!\n");

    terminal_setcolor(make_color(COLOR_WHITE, COLOR_BLACK));
    terminal_writestring("Kernel successfully loaded in 32-bit protected mode\n\n");

    /* Информация о системе */
    terminal_setcolor(make_color(COLOR_CYAN, COLOR_BLACK));
    terminal_writestring("System Information:\n");
    terminal_writestring("-------------------\n");

    terminal_setcolor(make_color(COLOR_YELLOW, COLOR_BLACK));
    terminal_writestring("* VGA Text Mode: 80x25\n");
    terminal_writestring("* Memory: 1MB conventional\n");
    terminal_writestring("* CPU: 32-bit protected mode\n");
    terminal_writestring("* Bootloader: Custom MBR\n\n");

    /* Инициализация клавиатуры */
    keyboard_init();
    terminal_writestring("Keyboard driver initialized\n");

    /* Запуск shell */
    terminal_writestring("\nStarting shell...\n\n");
    shell_start();

    /* Если shell завершится (чего не должно быть) */
    terminal_writestring("\n\nShell exited. System halted.\n");

    /* Бесконечный цикл */
    while (1) {
        /* Приостанавливаем процессор до прерывания */
        __asm__ volatile ("hlt");
    }
}
