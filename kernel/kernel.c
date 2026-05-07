#include <stdint.h>
#include <stdarg.h>
#include "drivers/keyboard.h"
#include "drivers/console.h"
#include "shell/shell.h"
#include "system/gdt.h"
#include "system/idt.h"

// --- Точка входа ядра ---
__attribute__((section(".text.prologue")))
void _start(BootInfo* bi) {
    asm volatile ("cli");

    initialize_console(bi);

    // Базовая CPU-инициализация: GDT и IDT
    gdt_init();
    idt_init();

    

    // Отображаем подробную системную информацию
    display_system_info(bi);

    // Инициализируем клавиатуру
    keyboard_init();
    
    // Обновляем статус клавиатуры
    current_color = convert_color(0x55FF55);
    printf("\n  Keyboard:         READY\n");
    
    current_color = convert_color(0x00AAFF); // Голубой
    printf("\n================================================\n");
    printf("  Type 'help' for available commands\n");
    printf("================================================\n\n");
    
    current_color = convert_color(0xFFFFFF);
    show_prompt();
    
    // Рисуем начальный курсор
    draw_cursor();
    
    while (1) {
        // Проверяем клавиатуру
        keyboard_handler();
        
        // Обновляем курсор (мигание)
        update_cursor();
        
        __asm__ volatile ("pause");
    }
}