/* string.c - функции для работы со строками */

#include "string.h"

/* Вычислить длину строки */
size_t strlen(const char *str) {
    size_t len = 0;

    while (str[len] != '\0') {
        len++;
    }

    return len;
}

/* Копировать строку */
char *strcpy(char *dest, const char *src) {
    char *orig_dest = dest;

    while (*src) {
        *dest++ = *src++;
    }

    *dest = '\0';
    return orig_dest;
}

/* Сравнить две строки */
int strcmp(const char *str1, const char *str2) {
    while (*str1 && (*str1 == *str2)) {
        str1++;
        str2++;
    }

    return (const unsigned char)*str1 - (const unsigned char)*str2;
}

/* Скопировать память */
void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }

    return dest;
}

/* Заполнить память значением */
void *memset(void *dest, int value, size_t n) {
    uint8_t *d = (uint8_t *)dest;

    for (size_t i = 0; i < n; i++) {
        d[i] = (uint8_t)value;
    }

    return dest;
}

/* Конвертировать число в строку */
char *itoa(int value, char *str, int base) {
    char *rc;
    char *ptr;
    char *low;

    if (base < 2 || base > 36) {
        *str = '\0';
        return str;
    }

    rc = ptr = str;

    if (value < 0 && base == 10) {
        *ptr++ = '-';
        rc++;
        value = -value;
    }

    low = ptr;

    do {
        *ptr++ =
            "zyxwvutsrqponmlkjihgfedcba9876543210123456789abcdefghijklmnopqrstuvwxyz"
            [35 + value % base];
        value /= base;
    } while (value);

    *ptr-- = '\0';

    while (low < ptr) {
        char tmp = *low;
        *low++ = *ptr;
        *ptr-- = tmp;
    }

    return rc;
}

/* Копировать строку с ограничением длины */
char *strncpy(char *dest, const char *src, size_t n) {
    char *original_dest = dest;
    size_t i;

    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }

    for (; i < n; i++) {
        dest[i] = '\0';
    }

    return original_dest;
}

/* Найти первое вхождение символа в строке */
char *strchr(const char *str, int ch) {
    while (*str != '\0') {
        if (*str == (char)ch) {
            return (char *)str;
        }
        str++;
    }

    return NULL;
}

/* Найти последнее вхождение символа в строке */
char *strrchr(const char *str, int ch) {
    const char *last = NULL;

    while (*str != '\0') {
        if (*str == (char)ch) {
            last = str;
        }
        str++;
    }

    if ((char)ch == '\0') {
        return (char *)str;
    }

    return (char *)last;
}

/* Объединить две строки */
char *strcat(char *dest, const char *src) {
    char *ptr = dest;

    /* Находим конец dest */
    while (*ptr != '\0') {
        ptr++;
    }

    /* Копируем src в конец dest */
    while (*src != '\0') {
        *ptr++ = *src++;
    }

    *ptr = '\0';
    return dest;
}

/* Разбить строку на токены (упрощенная версия) */
char *strtok(char *str, const char *delimiters) {
    static char *saved_token = NULL;
    char *token_start;

    /* Если передан новый указатель, используем его */
    if (str != NULL) {
        saved_token = str;
    } else if (saved_token == NULL) {
        return NULL;
    }

    /* Пропускаем начальные разделители */
    while (*saved_token != '\0') {
        const char *delim = delimiters;
        int is_delim = 0;

        while (*delim != '\0') {
            if (*saved_token == *delim) {
                is_delim = 1;
                break;
            }
            delim++;
        }

        if (!is_delim) {
            break;
        }

        saved_token++;
    }

    /* Если достигли конца строки */
    if (*saved_token == '\0') {
        saved_token = NULL;
        return NULL;
    }

    token_start = saved_token;

    /* Ищем конец токена */
    while (*saved_token != '\0') {
        const char *delim = delimiters;
        int is_delim = 0;

        while (*delim != '\0') {
            if (*saved_token == *delim) {
                is_delim = 1;
                break;
            }
            delim++;
        }

        if (is_delim) {
            *saved_token = '\0';
            saved_token++;
            break;
        }

        saved_token++;
    }

    if (*saved_token == '\0') {
        saved_token = NULL;
    }

    return token_start;
}

/* Сравнить две строки с ограничением длины */
int strncmp(const char *str1, const char *str2, size_t n) {
    while (n > 0 && *str1 && (*str1 == *str2)) {
        str1++;
        str2++;
        n--;
    }

    if (n == 0) {
        return 0;
    }

    return (const unsigned char)*str1 - (const unsigned char)*str2;
}
