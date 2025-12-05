/* string.h - функции для работы со строками */

#ifndef STRING_H
#define STRING_H

#include <stddef.h>
#include <stdint.h>

/* Вычислить длину строки */
size_t strlen(const char* str);

/* Копировать строку */
char* strcpy(char* dest, const char* src);

/* Сравнить две строки */
int strcmp(const char* str1, const char* str2);

/* Скопировать память */
void* memcpy(void* dest, const void* src, size_t n);

/* Заполнить память значением */
void* memset(void* dest, int value, size_t n);

/* Конвертировать число в строку */
char* itoa(int value, char* str, int base);

#endif /* STRING_H */