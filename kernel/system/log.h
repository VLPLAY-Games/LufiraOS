#ifndef BOOTLOG_H
#define BOOTLOG_H

#include "../drivers/console.h"

// Цвета для логов (используем индексы из 16-цветной палитры)
#define LOG_COLOR_OK        COLOR_LIGHT_GREEN    // ярко-зелёный
#define LOG_COLOR_FAIL      COLOR_LIGHT_RED      // ярко-красный
#define LOG_COLOR_WARN      COLOR_YELLOW         // жёлтый
#define LOG_COLOR_INFO      COLOR_WHITE          // белый
#define LOG_COLOR_PENDING   COLOR_LIGHT_CYAN     // голубой для статуса ожидания
#define LOG_COLOR_HEADER    COLOR_LIGHT_CYAN     // голубой для заголовков
#define LOG_COLOR_LABEL     COLOR_DARK_GRAY      // тёмно-серый для подписей

// Макросы для статусных сообщений (принимают строку как printf)
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

#define LOG_INFO(fmt, ...) do { \
    set_foreground_color(LOG_COLOR_INFO); \
    printf(fmt, ##__VA_ARGS__); \
    printf("\n"); \
} while(0)

// Специальный макрос для статуса "в процессе" (без перевода строки!)
#define LOG_PENDING(fmt, ...) do { \
    set_foreground_color(LOG_COLOR_PENDING); \
    printf("[  ....  ] "); \
    set_foreground_color(LOG_COLOR_INFO); \
    printf(fmt, ##__VA_ARGS__); \
} while(0)

// Завершить pending-строку успехом
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

// Пары "ключ: значение"
#define LOG_KV(key, fmt, ...) do { \
    set_foreground_color(LOG_COLOR_LABEL); \
    printf("  %-18s: ", key); \
    set_foreground_color(LOG_COLOR_INFO); \
    printf(fmt, ##__VA_ARGS__); \
    printf("\n"); \
} while(0)

#endif