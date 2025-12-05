/* string.c - функции для работы со строками */

#include "string.h"

/* Вычислить длину строки */
size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len] != '\0') {
        len++;
    }
    return len;
}

/* Копировать строку */
char* strcpy(char* dest, const char* src) {
    char* orig_dest = dest;
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
    return orig_dest;
}

/* Сравнить две строки */
int strcmp(const char* str1, const char* str2) {
    while (*str1 && (*str1 == *str2)) {
        str1++;
        str2++;
    }
    return *(const unsigned char*)str1 - *(const unsigned char*)str2;
}

/* Скопировать память */
void* memcpy(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    
    return dest;
}

/* Заполнить память значением */
void* memset(void* dest, int value, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    
    for (size_t i = 0; i < n; i++) {
        d[i] = (uint8_t)value;
    }
    
    return dest;
}

/* Конвертировать число в строку */
char* itoa(int value, char* str, int base) {
    char* rc;
    char* ptr;
    char* low;
    
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
        *ptr++ = "zyxwvutsrqponmlkjihgfedcba9876543210123456789abcdefghijklmnopqrstuvwxyz"[35 + value % base];
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
char strncpy(char* dest, const char* src, size_t n) {
    char* original_dest = dest;
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for ( ; i < n; i++) {
        dest[i] = '\0';
    }

    return original_dest;
}