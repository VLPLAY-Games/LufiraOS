/* kernel.c - основное ядро LufiraOS */

#include <stdint.h>
#include "../drivers/keyboard.h"
#include "shell.h"
#include "../fs/fs.h"
#include "../fs/disk.h"
#include "memory.h"

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

    /* Детекция памяти */
    memory_detect();
    
    /* Выводим информацию о памяти */
    const memory_map_t* mem_map = memory_get_map();
    char mem_buf[32];
    
    if (mem_map->entry_count > 0) {
        terminal_writestring("OK\n");
        
        /* Выводим общий объем памяти */
        uint32_t total_kb = (uint32_t)(memory_get_total() / 1024);
        uint32_t used_kb = (uint32_t)(memory_get_used() / 1024);
        uint32_t available_kb = (uint32_t)(memory_get_available() / 1024);
        
        terminal_writestring("[MEM] Total RAM: ");
        itoa(total_kb, mem_buf, 10);
        terminal_writestring(mem_buf);
        terminal_writestring(" KB\n");
        
        terminal_writestring("[MEM] Used: ");
        itoa(used_kb, mem_buf, 10);
        terminal_writestring(mem_buf);
        terminal_writestring(" KB (");
        if (total_kb > 0) {
            itoa((used_kb * 100) / total_kb, mem_buf, 10);
            terminal_writestring(mem_buf);
            terminal_writestring("%)\n");
        }
        
        terminal_writestring("[MEM] Available: ");
        itoa(available_kb, mem_buf, 10);
        terminal_writestring(mem_buf);
        terminal_writestring(" KB (");
        if (total_kb > 0) {
            itoa((available_kb * 100) / total_kb, mem_buf, 10);
            terminal_writestring(mem_buf);
            terminal_writestring("%)\n");
        }
    } else {
        terminal_writestring("FAILED - using default 640KB\n");
    }
    
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
        if (fs_format() == 0) {
            terminal_writestring("OK\n");
            
            /* После форматирования нужно переинициализировать ФС */
            terminal_writestring("[FS]  Reinitializing filesystem... ");
            fs_init();
            if (fs_is_initialized()) {
                terminal_writestring("OK\n");
            } else {
                terminal_writestring("FAILED\n");
            }
        } else {
            terminal_writestring("FAILED\n");
        }
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
    terminal_writestring(" bytes (");
    itoa(free_space / 1024, info_buf, 10);
    terminal_writestring(info_buf);
    terminal_writestring(" KB)\n");

    /* Запуск shell */
    terminal_setcolor(make_color(COLOR_LIGHT_GREEN, COLOR_BLACK));
    terminal_writestring("\nStarting shell...\n\n");
    terminal_setcolor(make_color(COLOR_WHITE, COLOR_BLACK));

    shell_start();

    /* Если shell завершится (чего не должно быть) */
    terminal_writestring("\n\nShell exited. System halted.\n");

    /* Бесконечный цикл */
    while (1) {
        __asm__ volatile ("hlt");
    }
}