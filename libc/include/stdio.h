// stdio.h — printf() плюс (sn)vsnprintf(). Никаких файловых потоков/scanf:
// файлы это по-прежнему напрямую sys_open/sys_read/sys_lseek/sys_close
// (lufira/syscall.h), не FILE*.
#pragma once

#include <stdarg.h>
#include <stddef.h>

int printf(const char *fmt, ...);

// Для вывода в буфер, а не на stdout. Поддержаны: %d %i %u %x %X %c %s %%
// %p, модификатор l/ll, ширина (включая 0-паддинг) и precision у целых/%s.
// Без флагов '+'/' '/'#', без %f. При переполнении size ведёт себя как
// настоящий snprintf: возвращает, сколько ушло бы байт без усечения, и
// всегда 0-терминирует результат, если size > 0.
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);
