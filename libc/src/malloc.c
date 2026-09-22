// malloc.c — минимальный аллокатор поверх sys_mmap()/sys_munmap().
//
// Ядро выделяет mmap-регионы ЖАДНО и уже ОБНУЛЯЕТ каждую страницу при
// выделении (см. комментарий у sys_mmap() в kernel/system/syscall/
// syscall.c: "Анонимная память обязана приходить обнулённой... на неё
// будет полагаться malloc() будущей libc") — поэтому здесь НЕТ ни одного
// memset(): полагаемся на эту гарантию вместо того, чтобы обнулять ещё раз.
//
// MAX_MMAP_REGIONS в ядре (kernel/system/process/process.h) — это ЖЁСТКИЙ
// потолок на число ОДНОВРЕМЕННО живых mmap-регионов процесса, общий с
// любым mmap(), который программа делает напрямую — не приватный бюджет
// malloc(). Вызывать mmap() на каждый malloc() исчерпал бы его уже после
// ~32 аллокаций в любом реальном тесте. Поэтому память берётся крупными
// "аренами" по ARENA_SIZE и внутри них раздаётся мелкими блоками.
#include <stdlib.h>
#include <stdint.h>
#include <lufira/syscall.h>

#define ARENA_SIZE       (64u * 1024u)   // 16 страниц — см. обоснование размера в плане
#define MAX_ARENAS       32                // = MAX_MMAP_REGIONS в ядре
#define PAGE_SIZE_LOCAL  4096u
#define SPLIT_THRESHOLD  64                 // не дробим блок, если остаток меньше этого

#define BLOCK_MAGIC_FREE  0xF2EEF2EEu
#define BLOCK_MAGIC_USED  0x05EDA110u
// Отдельный запрос, не влезающий в арену — выделен СВОИМ mmap() напрямую
// (не подблок арены). free() для такого блока обязан вызвать munmap() с
// РОВНО той же длиной, что была передана в mmap() — sys_munmap() требует
// точного совпадения (addr,length), частичный/поддиапазонный unmap не
// поддерживается (см. syscall.c) — поэтому size здесь хранит именно эту
// точную длину, а не запрошенный пользователем размер.
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

// Первый подходящий свободный блок — просматривает арены по порядку их
// создания, внутри каждой идёт по списку блоков (список никогда не
// пересекает границу арены — блоки только создаются/дробятся внутри
// new_arena()/split_block(), так что отдельно проверять границы не нужно).
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

// Сливает b со следующим блоком, если тот тоже свободен. Не трогает
// b->prev — вызывающий может после этого безопасно попытаться слить
// b->prev с (уже возможно укрупнённым) b тем же способом, получая слияние
// в обе стороны за два простых прохода вместо отдельной обратной логики.
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
