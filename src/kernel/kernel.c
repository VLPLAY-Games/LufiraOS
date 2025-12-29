#include "kernel.h"
#include "kernel_types.h"

// VGA буфер
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_ADDRESS 0xB8000

static uint16_t* vga_buffer = (uint16_t*)VGA_ADDRESS;
static size_t vga_row = 0;
static size_t vga_column = 0;
static uint8_t vga_color = 0x0F; // Белый на черном

// Создаем VGA символ
static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | (uint16_t)color << 8;
}

// Очистка экрана
void vga_clear(void) {
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            vga_buffer[index] = vga_entry(' ', vga_color);
        }
    }
    vga_row = 0;
    vga_column = 0;
}

// Установка цвета
void vga_set_color(uint8_t fg, uint8_t bg) {
    vga_color = fg | (bg << 4);
}

// Вывод символа
void vga_putchar(char c) {
    if (c == '\n') {
        vga_column = 0;
        if (++vga_row == VGA_HEIGHT) {
            vga_row = 0;
        }
        return;
    }
    
    const size_t index = vga_row * VGA_WIDTH + vga_column;
    vga_buffer[index] = vga_entry(c, vga_color);
    
    if (++vga_column == VGA_WIDTH) {
        vga_column = 0;
        if (++vga_row == VGA_HEIGHT) {
            vga_row = 0;
        }
    }
}

// Вывод строки
void vga_puts(const char *str) {
    while (*str) {
        vga_putchar(*str++);
    }
}

// Простые утилиты
size_t strlen(const char *str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

void memcpy(void *dest, const void *src, size_t n) {
    char *d = (char *)dest;
    const char *s = (const char *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

void memset(void *ptr, int value, size_t n) {
    char *p = (char *)ptr;
    for (size_t i = 0; i < n; i++) {
        p[i] = (char)value;
    }
}

// Инициализация ядра
void kernel_initialize(void) {
    vga_clear();
    vga_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    vga_puts("LufiraOS Kernel Initialized\n");
    vga_set_color(COLOR_LIGHT_GREY, COLOR_BLACK);
}

// Паника ядра
void kernel_panic(const char *message) {
    vga_set_color(COLOR_LIGHT_RED, COLOR_BLACK);
    vga_puts("\nKERNEL PANIC: ");
    vga_puts(message);
    vga_puts("\nSystem halted.\n");
    
    while(1) {
        __asm__("hlt");
    }
}

// Главная функция ядра
u64 kernel_main(void) {
    // Инициализация
    kernel_initialize();
    
    // Выводим информацию
    vga_puts("Welcome to LufiraOS!\n");
    vga_puts("====================\n\n");
    
    // Простая демонстрация
    vga_set_color(COLOR_CYAN, COLOR_BLACK);
    vga_puts("Kernel is running in 64-bit mode\n");
    
    vga_set_color(COLOR_YELLOW, COLOR_BLACK);
    vga_puts("Memory functions available\n");
    
    // Тест функций памяти
    char test_buffer[20];
    memset(test_buffer, 'A', 19);
    test_buffer[19] = '\0';
    
    vga_set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
    vga_puts("Test buffer: ");
    vga_puts(test_buffer);
    vga_puts("\n");
    
    vga_set_color(COLOR_WHITE, COLOR_BLACK);
    vga_puts("\nSystem ready. Press Ctrl+Alt+Del to reboot.\n");
    
    // Возвращаем магическое число
    return 0xDEADBEEFCAFEBABE;
}