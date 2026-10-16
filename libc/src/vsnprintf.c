// vsnprintf.c — отформатированная строка В БУФЕР, а не на stdout (см.
// комментарий в stdio.h). Не переиспользует буферный движок printf.c
// (buf_putc/buf_putuint и т.д. — тот источник пишет в фиксированный
// внутренний буфер строки один раз в конце, здесь же нужен настоящий
// snprintf с ограничением по size и возвратом "сколько бы ушло байт без
// усечения") — минимальное дублирование десятка строк форматирования
// дешевле, чем городить общий движок ради одного общего потребителя.
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>

typedef struct {
    char *buf;
    size_t size;  // вместимость buf, включая место под '\0'
    size_t pos;   // сколько символов "записано" ВСЕГО (может быть > size — как у настоящего snprintf)
} sink_t;

static void sink_putc(sink_t *sk, char c) {
    // size-1 слотов под сам текст, последний — под терминатор ниже.
    if (sk->pos + 1 < sk->size) sk->buf[sk->pos] = c;
    sk->pos++;
}

static void sink_puts(sink_t *sk, const char *s, int max_len) {
    for (int i = 0; s[i] && (max_len < 0 || i < max_len); i++) sink_putc(sk, s[i]);
}

static void sink_pad(sink_t *sk, int count, char padc) {
    while (count-- > 0) sink_putc(sk, padc);
}

// Раскладывает |value| в tmp (младшим разрядом вперёд) и возвращает число цифр.
static int format_uint(unsigned long value, int base, char *tmp) {
    static const char digits[] = "0123456789ABCDEF";
    int n = 0;
    if (value == 0) {
        tmp[n++] = '0';
    } else {
        while (value) {
            tmp[n++] = digits[value % (unsigned)base];
            value /= (unsigned)base;
        }
    }
    return n;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    sink_t sk = { buf, size, 0 };

    while (*fmt) {
        if (*fmt != '%') { sink_putc(&sk, *fmt++); continue; }
        fmt++;
        if (*fmt == '\0') break; // '%' в самом конце строки
        if (*fmt == '%') { sink_putc(&sk, '%'); fmt++; continue; }

        int zero_pad = 0, width = 0;
        int has_precision = 0, precision = 0;
        if (*fmt == '0') { zero_pad = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == '.') {
            fmt++;
            has_precision = 1;
            while (*fmt >= '0' && *fmt <= '9') { precision = precision * 10 + (*fmt - '0'); fmt++; }
        }

        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
            if (*fmt == 'l') fmt++; // %lld/%llu/%llx — long тут и так 64-битный
        }

        switch (*fmt) {
            case 'd':
            case 'i': {
                long v = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
                int neg = v < 0;
                unsigned long uv = neg ? (unsigned long)(-v) : (unsigned long)v;
                char tmp[32];
                int n = format_uint(uv, 10, tmp);
                int pad = width - (n + (neg ? 1 : 0));
                if (pad > 0 && !zero_pad) sink_pad(&sk, pad, ' ');
                if (neg) sink_putc(&sk, '-');
                if (pad > 0 && zero_pad) sink_pad(&sk, pad, '0');
                while (n > 0) sink_putc(&sk, tmp[--n]);
                break;
            }
            case 'u':
            case 'x':
            case 'X': {
                int base = (*fmt == 'u') ? 10 : 16;
                unsigned long v = is_long ? va_arg(ap, unsigned long)
                                          : (unsigned long)va_arg(ap, unsigned int);
                char tmp[32];
                int n = format_uint(v, base, tmp);
                if (*fmt == 'x') {
                    for (int i = 0; i < n; i++) {
                        if (tmp[i] >= 'A' && tmp[i] <= 'F') tmp[i] += ('a' - 'A');
                    }
                }
                int pad = width - n;
                if (pad > 0 && !zero_pad) sink_pad(&sk, pad, ' ');
                if (pad > 0 && zero_pad) sink_pad(&sk, pad, '0');
                while (n > 0) sink_putc(&sk, tmp[--n]);
                break;
            }
            case 'p': {
                void *v = va_arg(ap, void *);
                char tmp[32];
                int n = format_uint((unsigned long)v, 16, tmp);
                sink_puts(&sk, "0x", -1);
                while (n > 0) sink_putc(&sk, tmp[--n]);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                int len = 0;
                while (s[len]) len++;
                int use_len = (has_precision && precision < len) ? precision : len;
                int pad = width - use_len;
                if (pad > 0) sink_pad(&sk, pad, ' '); // только выравнивание вправо — флаг '-' не поддержан
                sink_puts(&sk, s, use_len);
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                sink_putc(&sk, c);
                break;
            }
            default:
                sink_putc(&sk, '%');
                sink_putc(&sk, *fmt);
                break;
        }
        fmt++;
    }

    if (size > 0) {
        size_t term = (sk.pos < size) ? sk.pos : size - 1;
        buf[term] = '\0';
    }
    return (int)sk.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}
