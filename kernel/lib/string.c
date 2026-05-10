#include "string.h"

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