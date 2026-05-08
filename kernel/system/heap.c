#include "heap.h"
#include "paging.h"
#include "pmm.h"
#include "../drivers/console.h"
#include <stdint.h>

#define KERNEL_HEAP_START   0xFFFF900000000000ULL
#define KERNEL_HEAP_INITIAL_SIZE   (4 * 1024 * 1024)   // 4 MB

typedef struct block_header {
    size_t size;                  // размер блока (включая заголовок)
    int free;                     // 1 если свободен
    struct block_header *next;
    struct block_header *prev;
} __attribute__((packed)) block_header_t;

static block_header_t *heap_start = NULL;
static block_header_t *heap_end = NULL;   // указатель на начало свободного виртуального пространства

static uint64_t heap_current_top = 0;      // текущий верхний виртуальный адрес

static block_header_t *find_free_block(size_t size) {
    block_header_t *current = heap_start;
    while (current) {
        if (current->free && current->size >= size) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

static block_header_t *request_space(block_header_t *last, size_t size) {
    // Выделяем новую страницу (или несколько) виртуальной памяти
    // и добавляем в список свободных как один большой блок.
    // Возвращаем указатель на начало выделенного блока (заголовок)
    uint64_t phys = pmm_alloc_page();
    if (!phys) return NULL;
    if (heap_current_top == 0) heap_current_top = KERNEL_HEAP_START;
    uint64_t virt = heap_current_top;
    if (map_page(virt, phys, PAGE_PRESENT | PAGE_WRITE) != 0) {
        pmm_free_page(phys);
        return NULL;
    }
    heap_current_top += PAGE_SIZE;

    // Инициализируем заголовок в начале страницы
    block_header_t *header = (block_header_t*)virt;
    header->size = PAGE_SIZE;
    header->free = 1;
    header->next = NULL;
    header->prev = last;
    if (last) last->next = header;

    // Если это первая страница, обновляем heap_start
    if (!heap_start) heap_start = header;
    heap_end = header;
    return header;
}

void heap_init(void) {
    printf("Heap initialized at 0x%lx\n", KERNEL_HEAP_START);
    heap_start = NULL;
    heap_end = NULL;
    heap_current_top = KERNEL_HEAP_START;
    // Можно сразу выделить начальный блок
    request_space(NULL, 0);
}

static void split_block(block_header_t *block, size_t size) {
    if (block->size - size > sizeof(block_header_t)) {
        block_header_t *new_block = (block_header_t*)((uint8_t*)block + size);
        new_block->size = block->size - size;
        new_block->free = 1;
        new_block->next = block->next;
        new_block->prev = block;
        if (block->next) block->next->prev = new_block;
        block->next = new_block;
        block->size = size;
    }
}

static void merge_blocks(block_header_t *block) {
    if (block->next && block->next->free) {
        block->size += block->next->size;
        block->next = block->next->next;
        if (block->next) block->next->prev = block;
    }
    if (block->prev && block->prev->free) {
        block->prev->size += block->size;
        block->prev->next = block->next;
        if (block->next) block->next->prev = block->prev;
        block = block->prev;
    }
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    size_t total_size = size + sizeof(block_header_t);
    // Выравнивание по 8 байт
    total_size = (total_size + 7) & ~7;

    block_header_t *block = find_free_block(total_size);
    if (!block) {
        block = request_space((heap_end) ? heap_end : NULL, total_size);
        if (!block) return NULL;
        // Если запрошенного блока недостаточно, будем расширять кучу дальше
        while (block->size < total_size) {
            if (!request_space(block, total_size - block->size)) {
                return NULL;
            }
            // После request_space у нас появился новый блок в конце, который свободен,
            // попробуем объединить с текущим
            merge_blocks(block);
            if (block->size >= total_size) break;
        }
    }

    split_block(block, total_size);
    block->free = 0;
    return (void*)((uint8_t*)block + sizeof(block_header_t));
}

void kfree(void *ptr) {
    if (!ptr) return;
    block_header_t *block = (block_header_t*)((uint8_t*)ptr - sizeof(block_header_t));
    block->free = 1;
    merge_blocks(block);
}