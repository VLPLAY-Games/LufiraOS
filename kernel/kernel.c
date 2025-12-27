/* kernel.c - основное ядро LufiraOS */

#include <stdint.h>
#include "../drivers/keyboard.h"
#include "shell.h"
#include "../fs/fs.h"
#include "../fs/disk.h"

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
    terminal_writestring("===================\n");

    terminal_setcolor(make_color(COLOR_YELLOW, COLOR_BLACK));
    terminal_writestring("* VGA Text Mode: 80x25\n");
    terminal_writestring("* Memory: 1MB conventional\n");
    terminal_writestring("* CPU: 32-bit protected mode\n");
    terminal_writestring("* Bootloader: Custom MBR\n");
    terminal_writestring("* Kernel Version: 0.1.0\n");
    terminal_writestring("* Architecture: i386\n");
    
    /* Информация о времени сборки */
    terminal_setcolor(make_color(COLOR_LIGHT_GREY, COLOR_BLACK));
#ifdef __DATE__
    terminal_writestring("* Build Date: ");
    terminal_writestring(__DATE__);
    terminal_writestring("\n");
#endif
#ifdef __TIME__
    terminal_writestring("* Build Time: ");
    terminal_writestring(__TIME__);
    terminal_writestring("\n");
#endif
    
    terminal_writestring("\n");
    
    /* Информация об инициализации драйверов */
    terminal_writestring("Initialization Log:\n");
    terminal_writestring("==================\n");

    /* Инициализация клавиатуры */
    terminal_writestring("[KBD] Initializing keyboard driver... ");
    keyboard_init();
    terminal_writestring("OK\n");

    /* Инициализация диска */
    terminal_writestring("[DSK] Initializing disk controller... ");
    disk_init();
    terminal_writestring("OK\n");

    /* Инициализация файловой системы */
    terminal_writestring("[FS]  Initializing filesystem... ");
    fs_init();
    
    /* Проверяем, нужно ли форматировать */
    if (!fs_is_initialized()) {
        terminal_writestring("NOT FOUND\n");
        terminal_writestring("[FS]  Filesystem not found, formatting... ");
        fs_format();
        terminal_writestring("OK\n");
    } else {
        terminal_writestring("OK\n");
        terminal_writestring("[FS]  Mounted successfully\n");
    }

    /* Информация о файловой системе */
    char info_buf[64];
    uint32_t free_space = fs_free_space();
    terminal_writestring("[FS]  Free space: ");
    itoa(free_space, info_buf, 10);
    terminal_writestring(info_buf);
    terminal_writestring(" bytes\n");
    
    /* Информация о памяти */
    terminal_writestring("[MEM] Kernel size: ~16KB\n");
    terminal_writestring("[MEM] Stack size: 16KB\n");
    terminal_writestring("[MEM] Available: ~960KB\n");

    /* Запуск shell */
    terminal_setcolor(make_color(COLOR_LIGHT_GREEN, COLOR_BLACK));
    terminal_writestring("\nStarting shell...\n\n");
    terminal_setcolor(make_color(COLOR_WHITE, COLOR_BLACK));

    shell_start();

    /* Если shell завершится (чего не должно быть) */
    terminal_writestring("\n\nShell exited. System halted.\n");

    /* Бесконечный цикл */
    while (1) {
        /* Приостанавливаем процессор до прерывания */
        __asm__ volatile ("hlt");
    }
}