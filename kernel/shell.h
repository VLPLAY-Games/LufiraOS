/* shell.h - простой shell для LufiraOS */

#ifndef SHELL_H
#define SHELL_H

#include <stdint.h>

/* Максимальная длина команды */
#define MAX_COMMAND_LENGTH 64
#define MAX_ARGUMENTS 10

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

/* Структура команды */
typedef struct {
    const char* name;
    const char* description;
    void (*handler)(void);
} command_t;

/* Вспомогательные функции для цветов */
static inline uint8_t make_color(enum vga_color fg, enum vga_color bg) {
    return (uint8_t)(fg | (bg << 4));
}

static inline uint16_t make_vgaentry(char c, uint8_t color) {
    uint16_t c16 = (uint8_t)c;
    uint16_t color16 = (uint8_t)color;
    return (uint16_t)(c16 | (color16 << 8));
}

/* Функции терминала */
void terminal_clear(void);
void terminal_writestring(const char* str);
void terminal_putchar(char c);
uint32_t terminal_get_y(void);
void terminal_set_cursor(uint32_t x, uint32_t y);
void terminal_init(void);
void update_hardware_cursor(void);
void disable_cursor(void);
void terminal_setcolor(uint8_t color);

/* Shell функции */
void shell_start(void);
void shell_execute(const char* command);

/* Команды */
void cmd_help(void);
void cmd_clear(void);
void cmd_shutdown(void);
void cmd_reboot(void);
void cmd_echo(void);
void cmd_info(void);

/* Глобальные переменные курсора (объявлены в kernel.c) */
extern uint32_t cursor_x;
extern uint32_t cursor_y;

#endif /* SHELL_H */