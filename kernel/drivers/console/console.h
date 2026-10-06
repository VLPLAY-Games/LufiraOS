#pragma once

#include "lib/types.h"
#include "lib/stddef.h"
#include "bootinfo.h"
#include "lib/colors.h"

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

// Двойная буферизация (v0.8-мост, пункт 2) — см. подробный комментарий в
// console.c. Включать ПОСЛЕ heap_init() И после того, как уже тикает
// таймер (kernel.c зовёт сразу после sti/irq_enable).
void console_enable_double_buffering(void);
// Копирует back buffer в hw-буфер немедленно, безусловно.
void gfx_present(void);
// Присутствует back buffer на экран, только если что-то рисовали с
// прошлого раза — зовётся из pit_timer_handler() каждый тик.
void console_tick_present(void);
// Для кода, который пишет в framebuffer[] напрямую в обход put_pixel()
// (graphics2d.c).
void console_mark_dirty(void);
void put_pixel(uint32_t x, uint32_t y, uint32_t color);
uint32_t convert_color(uint32_t color);
void put_char_graphic(int c, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color);
// v0.8 (GUI+WM) — см. комментарии у реализаций в console.c.
void put_char_graphic_px(int c, int x, int y, uint32_t fg_color, uint32_t bg_color);
void font_draw_glyph_to_buffer(uint32_t *buf, uint32_t buf_w, uint32_t buf_h,
                               int x, int y, int c, uint32_t fg_color);
// v0.8 (GUI+WM), этап 3: экспорт битмап-шрифта (full_font_data, static в
// console.c) для SYS_FB_FONT (syscall.c) — userspace WM-процесс (wm.c)
// больше не может звать put_char_graphic_px()/font_draw_glyph_to_buffer()
// напрямую (не его адресное пространство), ему нужны сырые байты глифов,
// чтобы рисовать титлбар/кнопку закрытия в СВОЕМ compositor-буфере. См.
// console_get_font_data() у реализации.
uint32_t console_get_font_size(void);
void console_get_font_data(void *out, uint32_t max_bytes);
void draw_text_scaled(const char *text, uint32_t px, uint32_t py, uint32_t scale, uint32_t fg_color);
void draw_text_tilted(const char *text, uint32_t px, uint32_t py, uint32_t scale, uint32_t fg_color);
uint32_t text_scaled_width(const char *text, uint32_t scale);
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

void console_scroll_up(void);
void console_scroll_down(void);
void console_scroll_to_bottom(void);
void console_redraw_from_history(void); // v0.8 (GUI+WM) - см. комментарий у реализации
int console_is_scrolled(void);
