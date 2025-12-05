/* kernel.c - основное ядро LufiraOS */

#include "keyboard.h"
#include "shell.h"
#include "string.h"

/* Типы данных */
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

/* Константы VGA */
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000

/* Цвета текста */
enum vga_color {
    COLOR_BLACK = 0,
    COLOR_BLUE = 1,
    COLOR_GREEN = 2,
    COLOR_CYAN = 3,
    COLOR_RED = 4,
    COLOR_MAGENTA = 5,
    COLOR_BROWN = 6,
    COLOR_LIGHT_GREY = 7,
    COLOR_DARK_GREY = 8,
    COLOR_LIGHT_BLUE = 9,
    COLOR_LIGHT_GREEN = 10,
    COLOR_LIGHT_CYAN = 11,
    COLOR_LIGHT_RED = 12,
    COLOR_LIGHT_MAGENTA = 13,
    COLOR_YELLOW = 14,
    COLOR_WHITE = 15,
};

/* Текущая позиция курсора */
static uint32_t cursor_x = 0;
static uint32_t cursor_y = 0;
static uint8_t terminal_color = 0x0F;  /* Белый на черном */

/* Получить цветовой атрибут */
static inline uint8_t make_color(enum vga_color fg, enum vga_color bg) {
    return fg | (bg << 4);
}

/* Получить VGA-символ */
static inline uint16_t make_vgaentry(char c, uint8_t color) {
    uint16_t c16 = c;
    uint16_t color16 = color;
    return c16 | (color16 << 8);
}

/* Очистка экрана */
void terminal_clear() {
    uint16_t* vga_buffer = (uint16_t*)VGA_MEMORY;
    uint16_t blank = make_vgaentry(' ', terminal_color);
    
    for (uint32_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga_buffer[i] = blank;
    }
    
    cursor_x = 0;
    cursor_y = 0;
}

/* Инициализация терминала */
void terminal_init() {
    terminal_clear();
}

/* Установить цвет текста */
void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

/* Поместить символ в определенную позицию */
void terminal_putentryat(char c, uint8_t color, uint32_t x, uint32_t y) {
    uint16_t* vga_buffer = (uint16_t*)VGA_MEMORY;
    uint32_t index = y * VGA_WIDTH + x;
    vga_buffer[index] = make_vgaentry(c, color);
}

/* Прокрутка экрана */
void terminal_scroll() {
    uint16_t* vga_buffer = (uint16_t*)VGA_MEMORY;
    
    // Копируем строки на одну вверх
    for (uint32_t y = 1; y < VGA_HEIGHT; y++) {
        for (uint32_t x = 0; x < VGA_WIDTH; x++) {
            uint32_t src_index = y * VGA_WIDTH + x;
            uint32_t dst_index = (y - 1) * VGA_WIDTH + x;
            vga_buffer[dst_index] = vga_buffer[src_index];
        }
    }
    
    // Очищаем последнюю строку
    uint16_t blank = make_vgaentry(' ', terminal_color);
    for (uint32_t x = 0; x < VGA_WIDTH; x++) {
        uint32_t index = (VGA_HEIGHT - 1) * VGA_WIDTH + x;
        vga_buffer[index] = blank;
    }
    
    cursor_y--;
}

/* Вывести символ */
void terminal_putchar(char c) {
    // Обработка перевода строки
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        
        if (cursor_y >= VGA_HEIGHT) {
            terminal_scroll();
        }
        return;
    }
    
    // Обработка возврата каретки
    if (c == '\r') {
        cursor_x = 0;
        return;
    }
    
    // Обработка табуляции
    if (c == '\t') {
        cursor_x = (cursor_x + 8) & ~(8 - 1);
        if (cursor_x >= VGA_WIDTH) {
            cursor_x = 0;
            cursor_y++;
        }
        return;
    }
    
    // Вывод обычного символа
    terminal_putentryat(c, terminal_color, cursor_x, cursor_y);
    
    cursor_x++;
    if (cursor_x >= VGA_WIDTH) {
        cursor_x = 0;
        cursor_y++;
        
        if (cursor_y >= VGA_HEIGHT) {
            terminal_scroll();
        }
    }
}

/* Вывести строку */
void terminal_writestring(const char* str) {
    while (*str) {
        terminal_putchar(*str++);
    }
}

/* Получить текущую позицию Y */
uint32_t terminal_get_y() {
    return cursor_y;
}

/* Установить позицию курсора */
void terminal_set_cursor(uint32_t x, uint32_t y) {
    cursor_x = x;
    cursor_y = y;
}

/* Главная функция ядра */
void kernel_main() {
    // Инициализация терминала
    terminal_init();
    
    // Приветственное сообщение
    terminal_setcolor(make_color(COLOR_GREEN, COLOR_BLACK));
    terminal_writestring("Welcome to LufiraOS!\n");
    
    terminal_setcolor(make_color(COLOR_WHITE, COLOR_BLACK));
    terminal_writestring("Kernel successfully loaded in 32-bit protected mode\n\n");
    
    // Информация о системе
    terminal_setcolor(make_color(COLOR_CYAN, COLOR_BLACK));
    terminal_writestring("System Information:\n");
    terminal_writestring("-------------------\n");
    
    terminal_setcolor(make_color(COLOR_YELLOW, COLOR_BLACK));
    terminal_writestring("* VGA Text Mode: 80x25\n");
    terminal_writestring("* Memory: 1MB conventional\n");
    terminal_writestring("* CPU: 32-bit protected mode\n");
    terminal_writestring("* Bootloader: Custom MBR\n\n");
    
    // Инициализация клавиатуры
    keyboard_init();
    terminal_writestring("Keyboard driver initialized\n");
    
    // Запуск shell
    terminal_writestring("\nStarting shell...\n\n");
    shell_start();
    
    // Если shell завершится (чего не должно быть)
    terminal_writestring("\n\nShell exited. System halted.\n");
    
    // Бесконечный цикл
    while (1) {
        __asm__ volatile ("pause");
    }
}