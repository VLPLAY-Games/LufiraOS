#pragma once

#include "drivers/console/console.h"
#include "lib/colors.h"

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
