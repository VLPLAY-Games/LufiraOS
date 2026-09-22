// malloc_test.c — стресс-проверка аллокатора (libc/src/malloc.c):
// 1) держит одновременно живыми ~3000 мелких аллокаций (связаны в список
//    ПРЯМО В ВЫДЕЛЕННОЙ ПАМЯТИ — не в массиве на стеке: пользовательский
//    стек всего 16КБ, массив из тысяч указателей его бы переполнил).
//    3000 * ~64 байт (с заголовком, выровнено) ~= 192КБ — гарантированно
//    продавливает несколько ARENA_SIZE=64KiB арен, доказывая, что схема
//    "арена + подвыделение" реально не упирается в потолок ядра
//    MAX_MMAP_REGIONS=32, в отличие от наивного mmap-на-каждый-malloc;
// 2) набор одновременно живых аллокаций разного размера с уникальным
//    паттерном в каждой — проверка отсутствия наложения/порчи между ними;
// 3) освобождение половины и повторное выделение (задействует слияние
//    свободных блоков) — оставшиеся живые должны остаться нетронутыми.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STRESS_COUNT    3000
#define STRESS_OBJ_SIZE 40
#define LIVE_COUNT      24

int main(void) {
    int fail = 0;

    // --- 1: держим тысячи мелких блоков живыми одновременно, связав их в
    // список через первые 8 байт каждого блока (не на стеке) ---
    void *head = NULL;
    int allocated = 0;
    for (int i = 0; i < STRESS_COUNT; i++) {
        void *p = malloc(STRESS_OBJ_SIZE);
        if (!p) {
            printf("stress-alloc FAIL at %d\n", i);
            fail = 1;
            break;
        }
        *(void **)p = head;
        head = p;
        allocated++;
    }
    if (!fail && allocated == STRESS_COUNT) {
        printf("stress-alloc OK\n");
    }
    while (head) {
        void *next = *(void **)head;
        free(head);
        head = next;
    }
    if (!fail) printf("stress-free OK\n");

    // --- 2: набор живых аллокаций разного размера, уникальный паттерн ---
    void *ptrs[LIVE_COUNT];
    size_t sizes[LIVE_COUNT];
    for (int i = 0; i < LIVE_COUNT; i++) {
        size_t sz = 16 + ((size_t)i * 37) % 500;
        sizes[i] = sz;
        ptrs[i] = malloc(sz);
        if (!ptrs[i]) {
            printf("live-alloc FAIL at %d\n", i);
            fail = 1;
            break;
        }
        memset(ptrs[i], i + 1, sz);
    }

    int corrupt = 0;
    for (int i = 0; i < LIVE_COUNT && !corrupt; i++) {
        unsigned char *p = (unsigned char *)ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            if (p[j] != (unsigned char)(i + 1)) { corrupt = 1; break; }
        }
    }
    if (corrupt) {
        printf("live-pattern-check FAIL\n");
        fail = 1;
    } else {
        printf("live-pattern-check OK\n");
    }

    // --- 3: освобождаем чётные, выделяем заново — нечётные не должны пострадать ---
    for (int i = 0; i < LIVE_COUNT; i += 2) {
        free(ptrs[i]);
        ptrs[i] = NULL;
    }
    for (int i = 0; i < LIVE_COUNT; i += 2) {
        size_t sz = 8 + (size_t)i;
        void *p = malloc(sz);
        if (!p) {
            printf("realloc-after-free FAIL at %d\n", i);
            fail = 1;
            break;
        }
        memset(p, 0xAA, sz);
        ptrs[i] = p;
    }

    corrupt = 0;
    for (int i = 1; i < LIVE_COUNT && !corrupt; i += 2) {
        unsigned char *p = (unsigned char *)ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            if (p[j] != (unsigned char)(i + 1)) { corrupt = 1; break; }
        }
    }
    if (corrupt) {
        printf("odd-survivors-intact FAIL\n");
        fail = 1;
    } else {
        printf("odd-survivors-intact OK\n");
    }

    printf(fail ? "malloc_test FAILED\n" : "all tests done\n");
    return fail;
}
