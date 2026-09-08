#pragma once

#include "types.h"

// Строковые утилиты для парсинга команд
const char* skip_spaces(const char* s);
int token_length(const char* s);
int token_equals(const char* s, const char* word);
int atoi(const char* str);
int hex_to_int(const char* hex);

// Базовые функции работы с памятью/строками (freestanding-ядро без libc:
// это единственные их реализации во всём kernel/, всё остальное должно
// подключать этот заголовок вместо своих локальных копий memset/memcpy/...).
void* memset(void* s, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
void* memmove(void* dest, const void* src, size_t n);

size_t strlen(const char* s);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
char* strcpy(char* dest, const char* src);

char to_lower(char c);
int strcmp_case_insensitive(const char* s1, const char* s2);
