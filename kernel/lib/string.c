#include "string.h"

// Обе функции ниже копируют/заполняют словами (8 байт), когда могут, а не
// побайтово — найдено живым тестированием gfx_present() (console.c,
// v0.8-мост, пункт 2): побайтовый memcpy() framebuffer-размера (несколько
// МБ) внутри pit_timer_handler() (под cli, см. console_tick_present())
// занимал заметную долю секунды КАЖДЫЙ раз, когда экран "грязный" — на
// практике ощущалось как зависание всей системы (следующий таймерный тик
// не мог прийти, пока предыдущий не отпустит cli). Выравнивание — только
// ПРЕФИКС до первого 8-байтного адреса, остаток словами, хвост < 8 байт
// обратно по байту — классический приём, без него word-доступ к
// невыровненному адресу либо падает, либо (здесь, SSE/без -mgeneral-regs-
// only уже не актуально) просто медленнее.
void* memset(void* s, int c, size_t n) {
    unsigned char* p = (unsigned char*)s;
    unsigned char byte = (unsigned char)c;

    while (n > 0 && ((uintptr_t)p & 7)) { *p++ = byte; n--; }

    if (n >= 8) {
        uint64_t word = (uint64_t)byte * 0x0101010101010101ULL;
        uint64_t *pw = (uint64_t*)p;
        size_t words = n / 8;
        for (size_t i = 0; i < words; i++) pw[i] = word;
        p += words * 8;
        n -= words * 8;
    }

    while (n--) *p++ = byte;
    return s;
}

void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;

    // Словами копируем, только если оба указателя выравниваются на 8
    // ОДИНАКОВО (общий случай — framebuffer/kmalloc'нутая память почти
    // всегда хотя бы 8-байтно выровнена сама по себе) — иначе откатываемся
    // на честный побайтовый путь на всю длину, не пытаясь исхитриться с
    // разным сдвигом src/dest.
    if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while (n > 0 && ((uintptr_t)d & 7)) { *d++ = *s++; n--; }

        if (n >= 8) {
            uint64_t *dw = (uint64_t*)d;
            const uint64_t *sw = (const uint64_t*)s;
            size_t words = n / 8;
            for (size_t i = 0; i < words; i++) dw[i] = sw[i];
            d += words * 8;
            s += words * 8;
            n -= words * 8;
        }
    }

    while (n--) *d++ = *s++;
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;

    if (d == s || n == 0) return dest;

    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }

    return dest;
}

size_t strlen(const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    if (n == 0) return 0;
    while (--n && *s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

char* strcpy(char* dest, const char* src) {
    char* orig = dest;
    while (*src) *dest++ = *src++;
    *dest = '\0';
    return orig;
}

char to_lower(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

int strcmp_case_insensitive(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        char c1 = to_lower(*s1), c2 = to_lower(*s2);
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return to_lower(*s1) - to_lower(*s2);
}

const char* skip_spaces(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

int token_length(const char* s) {
    int len = 0;
    while (s[len] != '\0' && s[len] != ' ' && s[len] != '\t') len++;
    return len;
}

int token_equals(const char* s, const char* word) {
    int i = 0;
    while (word[i] != '\0' && s[i] == word[i]) i++;
    return word[i] == '\0' && (s[i] == '\0' || s[i] == ' ' || s[i] == '\t');
}

int is_help_flag(const char* args) {
    if (!args) return 0;
    return token_equals(args, "-help") || token_equals(args, "--help") || token_equals(args, "-h");
}

int atoi(const char* str) {
    int result = 0, sign = 1;
    if (*str == '-') { sign = -1; str++; }
    while (*str >= '0' && *str <= '9') { result = result * 10 + (*str - '0'); str++; }
    return sign * result;
}

int hex_to_int(const char* hex) {
    int result = 0;
    while (*hex) {
        char c = *hex;
        int digit = 0;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        result = result * 16 + digit;
        hex++;
    }
    return result;
}