// printf.c — простой printf: %d %u %x %s %c %% %p, плюс %l-вариант
// (%ld/%lu/%lx; второй 'l' в %lld/%llu/%llx просто съедается — long тут и
// так 64-битный). Без ширины/точности/флагов/выравнивания. Собирает всю
// строку в фиксированный буфер и делает ОДИН write() в конце — при
// переполнении буфера просто перестаёт писать дальше (как klog_format() в
// ядре, kernel/system/klog/klog.c — тот же "простой printf" стандарт уже
// принят в этом проекте).
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <lufira/syscall.h>

#define PRINTF_BUF_SIZE 256

static void buf_putc(char *buf, int *pos, char c) {
    if (*pos < PRINTF_BUF_SIZE - 1) buf[(*pos)++] = c;
}

static void buf_puts(char *buf, int *pos, const char *s) {
    while (*s) buf_putc(buf, pos, *s++);
}

static void buf_putuint(char *buf, int *pos, unsigned long value, int base) {
    static const char digits[] = "0123456789abcdef";
    char tmp[32];
    int n = 0;

    if (value == 0) {
        tmp[n++] = '0';
    } else {
        while (value) {
            tmp[n++] = digits[value % (unsigned)base];
            value /= (unsigned)base;
        }
    }
    while (n > 0) buf_putc(buf, pos, tmp[--n]);
}

static void buf_putint(char *buf, int *pos, long value) {
    if (value < 0) {
        buf_putc(buf, pos, '-');
        buf_putuint(buf, pos, (unsigned long)(-value), 10);
    } else {
        buf_putuint(buf, pos, (unsigned long)value, 10);
    }
}

int printf(const char *fmt, ...) {
    char buf[PRINTF_BUF_SIZE];
    int pos = 0;
    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            buf_putc(buf, &pos, *fmt++);
            continue;
        }
        fmt++; // пропускаем '%'

        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
            if (*fmt == 'l') fmt++; // %lld/%llu/%llx
        }

        switch (*fmt) {
            case 'd': {
                long v = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
                buf_putint(buf, &pos, v);
                break;
            }
            case 'u': {
                unsigned long v = is_long ? va_arg(ap, unsigned long)
                                          : (unsigned long)va_arg(ap, unsigned int);
                buf_putuint(buf, &pos, v, 10);
                break;
            }
            case 'x': {
                unsigned long v = is_long ? va_arg(ap, unsigned long)
                                          : (unsigned long)va_arg(ap, unsigned int);
                buf_putuint(buf, &pos, v, 16);
                break;
            }
            case 'p': {
                void *v = va_arg(ap, void *);
                buf_puts(buf, &pos, "0x");
                buf_putuint(buf, &pos, (unsigned long)v, 16);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                buf_puts(buf, &pos, s ? s : "(null)");
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                buf_putc(buf, &pos, c);
                break;
            }
            case '%':
                buf_putc(buf, &pos, '%');
                break;
            case '\0':
                goto done; // '%' в самом конце строки — не заходим за terminator
            default:
                buf_putc(buf, &pos, '%');
                buf_putc(buf, &pos, *fmt);
                break;
        }
        fmt++;
    }

done:
    va_end(ap);

    sys_write(1, buf, (unsigned long)pos);
    return pos;
}
