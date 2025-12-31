#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

// Глобальные переменные состояния консоли
extern uint32_t current_x;
extern uint32_t current_y;
extern uint32_t screen_width_chars;
extern uint32_t screen_height_chars;
extern uint32_t* framebuffer;
extern uint32_t current_color;
extern uint32_t current_bg_color;
extern uint32_t pixels_per_scan_line;

// Прототипы функций
void initialize_console(void* bi);
void put_pixel(uint32_t x, uint32_t y, uint32_t color);
void put_char_graphic(char c, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color);
void put_char(char c);
void print_string(const char* str);
void printf(const char* format, ...);
void clear_screen(void);
void scroll_screen(void);
void utoa(uint64_t value, char* buffer, int base);

#endif