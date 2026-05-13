#include "heap.h"
#include "paging.h"
#include "pmm.h"
#include "lib/types.h"
#include "log.h"

#define HEAP_MAGIC_FREE  0xDEADBEE1
#define HEAP_MAGIC_USED  0xDEADBEE2

typedef struct block_header {
    uint32_t magic;           // Магическое число для проверки целостности
    uint32_t size;            // Размер блока (включая заголовок)
    struct block_header *next;
    struct block_header *prev;
} __attribute__((packed)) block_header_t;

static block_header_t *heap_start = NULL;
static uint64_t heap_initialized = 0;

// Включение/выключение прерываний для heap
static inline void heap_lock(void) {
    asm volatile("cli");
}

static inline void heap_unlock(void) {
    asm volatile("sti");
}

// Инициализация кучи - выделяем ВСЮ память сразу
void heap_init(void) {
    LOG_PENDING("Initializing static heap...");
    
    // Запрещаем прерывания во время инициализации
    heap_lock();
    
    // Маппим всю область кучи заранее
    for (uint64_t virt = KERNEL_HEAP_START; virt < KERNEL_HEAP_END; virt += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            LOG_DONE_FAIL("Heap initialization failed: out of physical memory");
            heap_unlock();
            while(1) __asm__("hlt");
        }
        
        if (map_page(virt, phys, PAGE_PRESENT | PAGE_WRITE) != 0) {
            LOG_DONE_FAIL("Heap initialization failed: mapping error");
            heap_unlock();
            while(1) __asm__("hlt");
        }
    }
    
    // Создаём начальный блок
    heap_start = (block_header_t*)KERNEL_HEAP_START;
    heap_start->magic = HEAP_MAGIC_FREE;
    heap_start->size = KERNEL_HEAP_SIZE;
    heap_start->next = NULL;
    heap_start->prev = NULL;
    
    heap_initialized = 1;
    heap_unlock();
    
    LOG_DONE_OK("Static heap initialized: 0x%lx - 0x%lx (%d MB)", 
                KERNEL_HEAP_START, KERNEL_HEAP_END, KERNEL_HEAP_SIZE / (1024 * 1024));
}

// Поиск свободного блока достаточного размера
static block_header_t* find_free_block(size_t size) {
    block_header_t *current = heap_start;
    
    while (current) {
        if (current->magic == HEAP_MAGIC_FREE && current->size >= size) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;
}

// Разделение блока на две части
static void split_block(block_header_t *block, size_t size) {
    // Размер нового блока должен быть достаточно большим
    if (block->size >= size + sizeof(block_header_t) + 32) {
        block_header_t *new_block = (block_header_t*)((uint8_t*)block + size);
        
        new_block->magic = HEAP_MAGIC_FREE;
        new_block->size = block->size - size;
        new_block->next = block->next;
        new_block->prev = block;
        
        if (block->next) {
            block->next->prev = new_block;
        }
        
        block->next = new_block;
        block->size = size;
    }
}

// Объединение соседних свободных блоков
static void merge_blocks(block_header_t *block) {
    // Объединяем со следующим блоком
    if (block->next && block->next->magic == HEAP_MAGIC_FREE) {
        block->size += block->next->size;
        block->next = block->next->next;
        if (block->next) {
            block->next->prev = block;
        }
    }
    
    // Объединяем с предыдущим блоком
    if (block->prev && block->prev->magic == HEAP_MAGIC_FREE) {
        block->prev->size += block->size;
        block->prev->next = block->next;
        if (block->next) {
            block->next->prev = block->prev;
        }
        block = block->prev;
    }
}

// Выделение памяти
void *kmalloc(size_t size) {
    if (!heap_initialized || size == 0) {
        return NULL;
    }
    
    // Выравниваем размер до 8 байт (минимальное выравнивание)
    size = (size + 7) & ~7;
    size_t total_size = size + sizeof(block_header_t);
    
    heap_lock();
    
    block_header_t *block = find_free_block(total_size);
    
    if (!block) {
        heap_unlock();
        LOG_FAIL("kmalloc: out of memory (requested %u bytes)", (uint32_t)size);
        return NULL;
    }
    
    split_block(block, total_size);
    block->magic = HEAP_MAGIC_USED;
    
    void *ptr = (void*)((uint8_t*)block + sizeof(block_header_t));
    
    heap_unlock();
    
    return ptr;
}

// Освобождение памяти
void kfree(void *ptr) {
    if (!ptr) return;
    
    heap_lock();
    
    block_header_t *block = (block_header_t*)((uint8_t*)ptr - sizeof(block_header_t));
    
    // Проверка на валидность указателя
    if (block->magic != HEAP_MAGIC_USED) {
        LOG_FAIL("kfree: invalid pointer %p (magic=0x%x)", ptr, block->magic);
        heap_unlock();
        return;
    }
    
    block->magic = HEAP_MAGIC_FREE;
    merge_blocks(block);
    
    heap_unlock();
}

// Вспомогательная функция для отладки (опционально)
void heap_dump(void) {
    heap_lock();
    
    printf("\n=== HEAP DUMP ===\n");
    block_header_t *current = heap_start;
    int block_num = 0;
    
    while (current) {
        printf("Block %d: addr=0x%lx, size=%u, magic=0x%x, %s\n",
               block_num++,
               (uint64_t)current,
               current->size,
               current->magic,
               (current->magic == HEAP_MAGIC_FREE) ? "FREE" : 
               (current->magic == HEAP_MAGIC_USED) ? "USED" : "INVALID");
        current = current->next;
    }
    
    printf("================\n\n");
    heap_unlock();
}