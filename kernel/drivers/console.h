#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

// Структура BootInfo
typedef struct {
    uint64_t FrameBufferBase;
    uint64_t FrameBufferSize;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelsPerScanLine;
    uint32_t PixelFormat;  // 0 = RGB, 1 = BGR
    uint64_t TotalMemory;   // Общая память в байтах
    uint64_t MemoryMapSize; // Размер карты памяти
    void* MemoryMap;        // Указатель на карту памяти
    uint32_t MemoryMapDescriptorSize; // Размер дескриптора
} BootInfo;

// Глобальные переменные состояния консоли
extern uint32_t current_x;
extern uint32_t current_y;
extern uint32_t screen_width_chars;
extern uint32_t screen_height_chars;
extern uint32_t* framebuffer;
extern uint32_t current_color;
extern uint32_t current_bg_color;
extern uint32_t pixels_per_scan_line;
extern uint32_t screen_width_pixels;
extern uint32_t screen_height_pixels;
extern uint32_t pixel_format;

// Флаги и счетчики для мигающего курсора
extern int cursor_visible;
extern int cursor_enabled;
extern uint32_t cursor_blink_counter;
extern uint32_t cursor_blink_rate;

// Прототипы функций
void initialize_console(BootInfo* bi);
void put_pixel(uint32_t x, uint32_t y, uint32_t color);
uint32_t convert_color(uint32_t color);
void put_char_graphic(char c, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color);
void put_char(char c);
void print_string(const char* str);
void printf(const char* format, ...);
void clear_screen(void);
void clear_entire_screen(void);
void scroll_screen(void);
void utoa(uint64_t value, char* buffer, int base);
void itoa(int64_t value, char* buffer, int base);

// Функции для работы с курсором
void draw_cursor(void);
void erase_cursor(void);
void update_cursor(void);
void enable_cursor(int enabled);
void move_cursor_left(void);
void move_cursor_right(void);
void set_cursor_position(uint32_t x, uint32_t y);

// Функция для отображения системной информации
void display_system_info(BootInfo* bi);

#endif