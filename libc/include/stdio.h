// stdio.h — printf() плюс (sn)vsnprintf(). Никаких файловых потоков/scanf
// в этой версии (см. план v0.4 Phase 4: "простой printf", не полноценный
// stdio) — файлы это по-прежнему напрямую sys_open/sys_read/sys_lseek/
// sys_close (lufira/syscall.h), не FILE*.
#pragma once

#include <stdarg.h>
#include <stddef.h>

int printf(const char *fmt, ...);

// Добавлены для порта doomgeneric — там отформатированная строка в буфер
// (имена savegame/lump, HUD-текст) нужна на каждом шагу, а не только вывод
// на stdout. Поддержаны: %d %i %u %x %X %c %s %% %p, модификатор l/ll,
// ширина (включая 0-паддинг, "%02d") и precision у %s ("%.8s"). Без
// флагов '+'/' '/'#', без %f (Doom-код их не использует). При переполнении
// size ведёт себя как настоящий snprintf: возвращает, сколько ушло бы
// байт БЕЗ усечения, и всегда 0-терминирует результат, если size > 0.
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);
