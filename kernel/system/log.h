#ifndef LOG_H
#define LOG_H

#include "../drivers/console.h"

// Цвета для логов
#define LOG_COLOR_OK        COLOR_LIGHT_GREEN     // ярко-зелёный
#define LOG_COLOR_FAIL      COLOR_LIGHT_RED       // ярко-красный
#define LOG_COLOR_WARN      COLOR_YELLOW          // жёлтый
#define LOG_COLOR_INFO      COLOR_WHITE           // белый
#define LOG_COLOR_PENDING   COLOR_LIGHT_CYAN      // голубой для статуса ожидания
#define LOG_COLOR_HEADER    COLOR_LIGHT_MAGENTA
#define LOG_COLOR_DIM       COLOR_DARK_GRAY       // тёмно-серый для INFO

// Цвета для SYSTEM STATUS
#define STATUS_READY        COLOR_LIGHT_GREEN     // зелёный для READY
#define STATUS_NOT_READY    COLOR_LIGHT_RED       // красный для NOT DETECTED

// Для ярлыков (Labels) в SYSTEM STATUS
#define LOG_COLOR_LABEL     COLOR_LIGHT_CYAN      // голубой для подписей

// ========== МАКРОСЫ СТАТУСОВ ==========

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

#define LOG_INFO_LINE(fmt, ...) do { \
    set_foreground_color(LOG_COLOR_DIM); \
    printf("[ INFO ] "); \
    set_foreground_color(LOG_COLOR_DIM); \
    printf(fmt, ##__VA_ARGS__); \
    printf("\n"); \
    set_foreground_color(COLOR_WHITE); \
} while(0)

#define LOG_PENDING(fmt, ...) do { \
    set_foreground_color(LOG_COLOR_PENDING); \
    printf("[  ....  ] "); \
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

// Заголовки секций (киберпанк)
#define LOG_SECTION(title) do { \
    printf("\n"); \
    set_foreground_color(LOG_COLOR_HEADER); \
    printf("=== %s ===\n", title); \
    set_foreground_color(LOG_COLOR_INFO); \
} while(0)

// Разделитель (киберпанк)
#define LOG_SEPARATOR do { \
    set_foreground_color(LOG_COLOR_HEADER); \
    printf("================================================\n"); \
    set_foreground_color(LOG_COLOR_INFO); \
} while(0)

// Вывод значения статуса (зелёный/красный)
#define LOG_STATUS_LINE(label, status, fmt, ...) do { \
    set_foreground_color(LOG_COLOR_LABEL); \
    printf("  %s: ", label); \
    if (status) { \
        set_foreground_color(STATUS_READY); \
    } else { \
        set_foreground_color(STATUS_NOT_READY); \
    } \
    printf(fmt, ##__VA_ARGS__); \
    printf("\n"); \
} while(0)

#endif