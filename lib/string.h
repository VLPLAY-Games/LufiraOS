/* string.h - функции для работы со строками */

#ifndef STRING_H
#define STRING_H

#include <stddef.h>
#include <stdint.h>

/* Вычислить длину строки */
size_t strlen(const char *str);

/* Копировать строку */
char *strcpy(char *dest, const char *src);

/* Сравнить две строки */
int strcmp(const char *str1, const char *str2);

/* Скопировать память */
void *memcpy(void *dest, const void *src, size_t n);

/* Заполнить память значением */
void *memset(void *dest, int value, size_t n);

/* Конвертировать число в строку */
char *itoa(int value, char *str, int base);

/* Копировать строку с ограничением длины */
char *strncpy(char *dest, const char *src, size_t n);

/* Сравнить две строки с ограничением длины */
int strncmp(const char *str1, const char *str2, size_t n);

/* Найти первое вхождение символа в строке */
char *strchr(const char *str, int ch);

/* Найти последнее вхождение символа в строке */
char *strrchr(const char *str, int ch);

/* Объединить две строки */
char *strcat(char *dest, const char *src);

/* Разбить строку на токены */
char *strtok(char *str, const char *delimiters);

#endif /* STRING_H */
