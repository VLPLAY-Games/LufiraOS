// string_test.c — санити-проверка каждой функции из libc/src/string.c.
#include <stdio.h>
#include <string.h>

int main(void) {
    int fail = 0;
    char buf[64];

    // memcpy / memcmp
    memcpy(buf, "hello world", 12);
    if (memcmp(buf, "hello world", 12) != 0) { printf("memcpy FAIL\n"); fail = 1; }
    else printf("memcpy OK\n");

    // memset
    memset(buf, 'A', 5);
    if (buf[0] != 'A' || buf[4] != 'A' || buf[5] != ' ') { printf("memset FAIL\n"); fail = 1; }
    else printf("memset OK\n");

    // memmove (перекрывающиеся регионы)
    strcpy(buf, "abcdefgh");
    memmove(buf + 2, buf, 5); // "ab" + "abcde" -> "ababcdeh"
    if (memcmp(buf, "ababcde", 7) != 0) { printf("memmove FAIL\n"); fail = 1; }
    else printf("memmove OK\n");

    // strlen / strcpy
    strcpy(buf, "test string");
    if (strlen(buf) != 11) { printf("strlen/strcpy FAIL\n"); fail = 1; }
    else printf("strlen-strcpy OK\n");

    // strncpy
    char nbuf[8];
    strncpy(nbuf, "hi", sizeof(nbuf));
    if (nbuf[0] != 'h' || nbuf[1] != 'i' || nbuf[2] != '\0' || nbuf[7] != '\0') {
        printf("strncpy FAIL\n"); fail = 1;
    } else {
        printf("strncpy OK\n");
    }

    // strcmp / strncmp
    if (strcmp("abc", "abc") != 0 || strcmp("abc", "abd") >= 0 || strcmp("abd", "abc") <= 0) {
        printf("strcmp FAIL\n"); fail = 1;
    } else {
        printf("strcmp OK\n");
    }
    if (strncmp("abcXX", "abcYY", 3) != 0 || strncmp("abc", "abd", 3) >= 0) {
        printf("strncmp FAIL\n"); fail = 1;
    } else {
        printf("strncmp OK\n");
    }

    // strcat
    strcpy(buf, "foo");
    strcat(buf, "bar");
    if (strcmp(buf, "foobar") != 0) { printf("strcat FAIL\n"); fail = 1; }
    else printf("strcat OK\n");

    // strchr
    const char *s = "needle-in-haystack";
    char *found = strchr(s, '-');
    if (!found || found != s + 6) { printf("strchr FAIL\n"); fail = 1; }
    else printf("strchr OK\n");

    printf(fail ? "string_test FAILED\n" : "all tests done\n");
    return fail;
}
