#pragma once

// ========== 16-ЦВЕТНАЯ ПАЛИТРА (ИНДЕКСЫ) ==========
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
    COLOR_RGB
} ConsoleColor;

// ========== 16-ЦВЕТНАЯ ПАЛИТРА (RGB ЗНАЧЕНИЯ) ==========
#define RGB_BLACK           0x000000
#define RGB_BLUE            0x0000AA
#define RGB_GREEN           0x00AA00
#define RGB_CYAN            0x00AAAA
#define RGB_RED             0xAA0000
#define RGB_MAGENTA         0xAA00AA
#define RGB_BROWN           0xAA5500
#define RGB_LIGHT_GRAY      0xAAAAAA
#define RGB_DARK_GRAY       0x555555
#define RGB_LIGHT_BLUE      0x5555FF
#define RGB_LIGHT_GREEN     0x55FF55
#define RGB_LIGHT_CYAN      0x55FFFF
#define RGB_LIGHT_RED       0xFF5555
#define RGB_LIGHT_MAGENTA   0xFF55FF
#define RGB_YELLOW          0xFFFF55
#define RGB_WHITE           0xFFFFFF

// ========== ЦВЕТА ДЛЯ ЛОГОВ ==========
#define LOG_COLOR_OK        COLOR_LIGHT_GREEN
#define LOG_COLOR_FAIL      COLOR_LIGHT_RED
#define LOG_COLOR_WARN      COLOR_YELLOW
#define LOG_COLOR_INFO      COLOR_WHITE
#define LOG_COLOR_PENDING   COLOR_LIGHT_CYAN
#define LOG_COLOR_HEADER    COLOR_LIGHT_MAGENTA
#define LOG_COLOR_DIM       COLOR_DARK_GRAY
#define LOG_COLOR_LABEL     COLOR_LIGHT_CYAN

#define STATUS_READY        COLOR_LIGHT_GREEN
#define STATUS_NOT_READY    COLOR_LIGHT_RED