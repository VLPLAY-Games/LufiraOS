#pragma once
/*
 * Заглушка kernel/lib/string.h для хостовой сборки selftest (см. README).
 * Подменяется через -I. раньше настоящего kernel/, чтобы проверяемые .c
 * собирались БЕЗ единого #ifdef на два окружения.
 */
#include <string.h>

static inline char to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}
