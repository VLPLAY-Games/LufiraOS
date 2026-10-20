// malloc.c — минимальный аллокатор поверх sys_mmap()/sys_munmap().
//
// Ядро уже обнуляет каждую страницу при выделении mmap — поэтому здесь нет
// ни одного memset(): полагаемся на эту гарантию вместо повторного обнуления.
//
// MAX_MMAP_REGIONS в ядре — общий жёсткий потолок на число одновременно
// живых mmap-регионов процесса (не приватный бюджет malloc()). Вызов mmap()
// на каждый malloc() исчерпал бы его быстро, поэтому память берётся
// крупными "аренами" по ARENA_SIZE и раздаётся внутри них мелкими блоками.
#include <stdlib.h>
#include <stdint.h>
#include <lufira/syscall.h>

#define ARENA_SIZE       (64u * 1024u)   // 16 страниц — см. обоснование размера в плане
#define MAX_ARENAS       32                // = MAX_MMAP_REGIONS в ядре
#define PAGE_SIZE_LOCAL  4096u
#define SPLIT_THRESHOLD  64                 // не дробим блок, если остаток меньше этого

#define BLOCK_MAGIC_FREE  0xF2EEF2EEu
#define BLOCK_MAGIC_USED  0x05EDA110u
// Отдельный запрос, не влезающий в арену — выделен своим mmap() напрямую.
// free() для такого блока обязан вызвать munmap() с ровно той же длиной,
// что была передана в mmap() (sys_munmap() не поддерживает частичный
// unmap) — поэтому size здесь хранит точную длину mmap, а не запрошенный
// пользователем размер.
#define BLOCK_MAGIC_LARGE 0xA31DA31Du

typedef struct block_header {
    uint32_t magic;
    uint32_t size;   // весь блок целиком, включая этот заголовок
    struct block_header *next;
    struct block_header *prev;
} block_header_t;

typedef struct {
    void *base;
    block_header_t *first;
} arena_t;

static arena_t arenas[MAX_ARENAS];
static int arena_count = 0;

static block_header_t *new_arena(void) {
    if (arena_count >= MAX_ARENAS) return NULL;

    long r = sys_mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1);
    if (r < 0) return NULL;

    block_header_t *block = (block_header_t *)(void *)r;
    block->magic = BLOCK_MAGIC_FREE;
    block->size = ARENA_SIZE;
    block->next = NULL;
    block->prev = NULL;

    arenas[arena_count].base = (void *)r;
    arenas[arena_count].first = block;
    arena_count++;

    return block;
}

// Первый подходящий свободный блок: арены по порядку создания, внутри
// каждой — по списку блоков (список не пересекает границу арены).
static block_header_t *find_free_block(size_t need) {
    for (int i = 0; i < arena_count; i++) {
        for (block_header_t *b = arenas[i].first; b; b = b->next) {
            if (b->magic == BLOCK_MAGIC_FREE && b->size >= need) return b;
        }
    }
    return NULL;
}

static void split_block(block_header_t *b, size_t need) {
    size_t remaining = b->size - need;
    if (remaining < sizeof(block_header_t) + SPLIT_THRESHOLD) return; // остаток мал — не дробим

    block_header_t *tail = (block_header_t *)((uint8_t *)b + need);
    tail->magic = BLOCK_MAGIC_FREE;
    tail->size = (uint32_t)remaining;
    tail->next = b->next;
    tail->prev = b;
    if (b->next) b->next->prev = tail;
    b->next = tail;

    b->size = (uint32_t)need;
}

// Сливает b со следующим блоком, если тот тоже свободен. b->prev не
// трогает — вызывающий может затем слить b->prev с уже укрупнённым b тем
// же способом, получая слияние в обе стороны за два прохода.
static void merge_with_next(block_header_t *b) {
    block_header_t *n = b->next;
    if (n && n->magic == BLOCK_MAGIC_FREE) {
        b->size += n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
    }
}

void *malloc(size_t size) {
    if (size == 0) return NULL;

    size_t need = size + sizeof(block_header_t);
    need = (need + 15u) & ~(size_t)15u; // выравнивание под 16 байт

    // Крупный запрос — отдельный выделенный mmap, минуя арены целиком.
    if (need > ARENA_SIZE) {
        size_t total = (need + PAGE_SIZE_LOCAL - 1u) & ~(size_t)(PAGE_SIZE_LOCAL - 1u);
        long r = sys_mmap(NULL, total, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1);
        if (r < 0) return NULL;

        block_header_t *b = (block_header_t *)(void *)r;
        b->magic = BLOCK_MAGIC_LARGE;
        b->size = (uint32_t)total; // точная длина mmap — нужна free() для munmap()
        b->next = NULL;
        b->prev = NULL;
        return (void *)(b + 1);
    }

    block_header_t *b = find_free_block(need);
    if (!b) {
        b = new_arena();
        if (!b) return NULL;
    }

    split_block(b, need);
    b->magic = BLOCK_MAGIC_USED;
    return (void *)(b + 1);
}

// Сколько байт реально доступно по указателю (payload без заголовка, может
// быть больше запрошенного из-за выравнивания). Нужен сторонним
// realloc()-реализациям, которым иначе неоткуда узнать старый размер блока.
// Для свободного/битого указателя — 0, как и free() тихо игнорирует такой вход.
size_t malloc_usable_size(void *ptr) {
    if (!ptr) return 0;
    block_header_t *b = (block_header_t *)ptr - 1;
    if (b->magic != BLOCK_MAGIC_USED && b->magic != BLOCK_MAGIC_LARGE) return 0;
    return b->size - sizeof(block_header_t);
}

void free(void *ptr) {
    if (!ptr) return;

    block_header_t *b = (block_header_t *)ptr - 1;

    if (b->magic == BLOCK_MAGIC_LARGE) {
        sys_munmap(b, b->size); // b->size — точная длина, переданная в mmap()
        return;
    }

    if (b->magic != BLOCK_MAGIC_USED) return; // двойной free()/битый указатель — тихо игнорируем

    b->magic = BLOCK_MAGIC_FREE;

    merge_with_next(b);
    if (b->prev && b->prev->magic == BLOCK_MAGIC_FREE) {
        merge_with_next(b->prev);
    }
}
