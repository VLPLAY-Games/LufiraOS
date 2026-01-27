#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>
#include <stddef.h>

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

// Цветовая палитра - 16 стандартных цветов
typedef enum {
    COLOR_BLACK = 0,
    COLOR_BLUE,
    COLOR_GREEN,
    COLOR_CYAN,
    COLOR_RED,
    COLOR_MAGENTA,
    COLOR_BROWN,
    COLOR_LIGHT_GRAY,
    COLOR_DARK_GRAY,
    COLOR_LIGHT_BLUE,
    COLOR_LIGHT_GREEN,
    COLOR_LIGHT_CYAN,
    COLOR_LIGHT_RED,
    COLOR_LIGHT_MAGENTA,
    COLOR_YELLOW,
    COLOR_WHITE,
    COLOR_RGB // Для пользовательских RGB цветов
} ConsoleColor;

// Структура для хранения цветовых пар (текст/фон)
typedef struct {
    uint32_t fg_color;      // Цвет текста (RGB в формате фреймбуфера)
    uint32_t bg_color;      // Цвет фона (RGB в формате фреймбуфера)
    ConsoleColor fg_index;  // Индекс цвета текста в палитре
    ConsoleColor bg_index;  // Индекс цвета фона в палитре
} ColorPair;

// Глобальная текущая цветовая пара
extern ColorPair current_colors;

// Палитра из 16 стандартных цветов (в формате 0xRRGGBB)
extern const uint32_t color_palette_16[];
extern const char* color_names_16[];

// Палитра из 256 цветов (VGA/ANSI расширенная палитра)
extern uint32_t color_palette_256[];

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

// Функции для работы с цветами
void set_color_by_index(ConsoleColor fg, ConsoleColor bg);
void set_color_by_rgb(uint32_t fg_rgb, uint32_t bg_rgb);
void set_foreground_color(ConsoleColor color);
void set_background_color(ConsoleColor color);
void set_foreground_rgb(uint32_t rgb);
void set_background_rgb(uint32_t rgb);
void reset_colors(void);
void print_color_table_16(void);
uint32_t get_color_from_palette(int index);
ConsoleColor find_closest_color(uint32_t rgb);
void init_256_color_palette(void);
const char* get_color_name(ConsoleColor color);

// Функция для отображения системной информации
void display_system_info(BootInfo* bi);

#endif