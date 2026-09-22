#include <string.h>

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memset(void *dst, int value, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    unsigned char v = (unsigned char)value;
    for (size_t i = 0; i < n; i++) d[i] = v;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

char *strcpy(char *dst, const char *src) {
    char *ret = dst;
    while ((*dst++ = *src++)) { }
    return ret;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0') return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

char *strcat(char *dst, const char *src) {
    char *ret = dst;
    while (*dst) dst++;
    while ((*dst++ = *src++)) { }
    return ret;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}
