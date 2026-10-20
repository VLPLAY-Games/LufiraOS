// stdlib.h — минимальный набор: malloc/free (см. libc/src/malloc.c) и
// exit(). Никаких calloc/realloc: переиспользуемый блок может содержать
// старые данные, так что настоящий calloc() всё равно нуждается в memset —
// "тривиальной" экономии не получается, поэтому его здесь нет.
#pragma once

#include <stddef.h>

void *malloc(size_t size);
void free(void *ptr);

// Полезный размер блока по указателю (payload без заголовка) — не
// calloc()/realloc(), но достаточно, чтобы сторонний код мог реализовать
// собственный realloc() поверх malloc()/free(), зная, сколько байт
// копировать в новый блок. 0 для свежего/неизвестного указателя.
size_t malloc_usable_size(void *ptr);

// Поддержаны только основания 10 и 16 (с префиксом "0x"/"0X") — base=0
// автоопределяет между ними; endptr может быть NULL.
long strtol(const char *s, char **endptr, int base);

// Оборачивает SYS_EXIT — не возвращается. Никаких atexit()-хендлеров или
// сброса stdio-буферов: printf() делает write() сразу при каждом вызове,
// сбрасывать нечего.
__attribute__((noreturn)) void exit(int code);
