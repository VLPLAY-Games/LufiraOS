#include <stdlib.h>
#include <lufira/syscall.h>

void exit(int code) {
    sys_exit(code);
    for (;;) { } // sys_exit() не возвращается; чисто чтобы компилятор поверил noreturn
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static int digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

long strtol(const char *s, char **endptr, int base) {
    const char *orig = s;
    while (is_space(*s)) s++;

    int neg = 0;
    if (*s == '+' || *s == '-') {
        neg = (*s == '-');
        s++;
    }

    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = 10;
    }

    long value = 0;
    const char *start = s;
    for (int d; (d = digit_value(*s)) >= 0 && d < base; s++) {
        value = value * base + d;
    }

    if (endptr) *endptr = (char *)((s == start) ? orig : s);
    return neg ? -value : value;
}
