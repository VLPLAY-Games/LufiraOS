#ifndef BOOTLOG_H
#define BOOTLOG_H

#include "../drivers/console.h"

// Цвета для логов
#define LOG_COLOR_OK        COLOR_LIGHT_GREEN
#define LOG_COLOR_FAIL      COLOR_LIGHT_RED
#define LOG_COLOR_WARN      COLOR_YELLOW
#define LOG_COLOR_INFO      COLOR_WHITE
#define LOG_COLOR_PENDING   COLOR_LIGHT_CYAN
#define LOG_COLOR_HEADER    COLOR_LIGHT_CYAN
#define LOG_COLOR_LABEL     COLOR_DARK_GRAY

// Макросы для статусных сообщений
#define LOG_OK(fmt, ...) do { \
    set_foreground_color(LOG_COLOR_OK); \
    printf("[  OK  ] "); \
    set_foreground_color(LOG_COLOR_INFO); \
    printf(fmt, ##__VA_ARGS__); \
    printf("\n"); \
} while(0)

#define LOG_FAIL(fmt, ...) do { \
    set_foreground_color(LOG_COLOR_FAIL); \
    printf("[FAILED] "); \
    set_foreground_color(LOG_COLOR_INFO); \
    printf(fmt, ##__VA_ARGS__); \
    printf("\n"); \
} while(0)

#define LOG_WARN(fmt, ...) do { \
    set_foreground_color(LOG_COLOR_WARN); \
    printf("[ WARN ] "); \
    set_foreground_color(LOG_COLOR_INFO); \
    printf(fmt, ##__VA_ARGS__); \
    printf("\n"); \
} while(0)

// Информационная строка с префиксом [ INFO ]
#define LOG_INFO_LINE(fmt, ...) do { \
    set_foreground_color(COLOR_DARK_GRAY); \
    printf("[ INFO ] "); \
    set_foreground_color(COLOR_DARK_GRAY); \
    printf(fmt, ##__VA_ARGS__); \
    printf("\n"); \
    set_foreground_color(COLOR_WHITE); \
} while(0)

#define LOG_PENDING(fmt, ...) do { \
    set_foreground_color(LOG_COLOR_PENDING); \
    printf("[  ..  ] "); \
    set_foreground_color(LOG_COLOR_INFO); \
    printf(fmt, ##__VA_ARGS__); \
} while(0)

#define LOG_DONE_OK(fmt, ...) do { \
    printf("\r"); \
    set_foreground_color(LOG_COLOR_OK); \
    printf("[  OK  ] "); \
    set_foreground_color(LOG_COLOR_INFO); \
    printf(fmt, ##__VA_ARGS__); \
    printf("                              \n"); \
} while(0)

#define LOG_DONE_FAIL(fmt, ...) do { \
    printf("\r"); \
    set_foreground_color(LOG_COLOR_FAIL); \
    printf("[FAILED] "); \
    set_foreground_color(LOG_COLOR_INFO); \
    printf(fmt, ##__VA_ARGS__); \
    printf("                              \n"); \
} while(0)

#define LOG_DONE_WARN(fmt, ...) do { \
    printf("\r"); \
    set_foreground_color(LOG_COLOR_WARN); \
    printf("[ WARN ] "); \
    set_foreground_color(LOG_COLOR_INFO); \
    printf(fmt, ##__VA_ARGS__); \
    printf("                              \n"); \
} while(0)

// Заголовки секций
#define LOG_SECTION(title) do { \
    printf("\n"); \
    set_foreground_color(LOG_COLOR_HEADER); \
    printf("=== %s ===\n", title); \
    set_foreground_color(LOG_COLOR_INFO); \
} while(0)

#endif