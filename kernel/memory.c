/* memory.c - управление памятью для LufiraOS */

#include "memory.h"
#include "string.h"
#include "shell.h"

/* Глобальная карта памяти */
static memory_map_t memory_map = {0};

/* Для простой аллокации */
static uint8_t* heap_start = (uint8_t*)0x20000; /* Начинаем с 128KB */
static uint8_t* heap_current = (uint8_t*)0x20000;
static uint8_t* heap_end = (uint8_t*)0x9AFFF;   /* Куча до 0x9AFFF (около 480KB) */
static uint32_t heap_total_size = 0;            /* Общий размер кучи */
static uint32_t heap_used = 0;                  /* Использовано в куче */

/* Структура для отслеживания выделенных блоков */
typedef struct mem_block {
    uint32_t size;
    uint8_t used;
    struct mem_block* next;
} mem_block_t;

static mem_block_t* block_list = NULL;

/* Детекция памяти через BIOS вызовы (упрощенная версия) */
void memory_detect(void) {
    terminal_writestring("[MEM] Detecting memory... ");
    
    /* Для QEMU обычно доступно 128MB-256MB */
    /* Создаем простую карту для совместимости */
    
    /* 1. Первые 640KB - обычная память */
    memory_map.entry_count = 1;
    memory_map.entries[0].base_addr = 0x00000000;
    memory_map.entries[0].length = 0x0009F000;  /* 636KB */
    memory_map.entries[0].type = E820_TYPE_AVAILABLE;
    memory_map.entries[0].extended = 0;
    
    /* Инициализируем общие счетчики */
    memory_map.total_memory = memory_map.entries[0].length;
    memory_map.available_memory = memory_map.entries[0].length - 0x20000; /* Минус область под кучу */
    
    /* Инициализируем кучу */
    heap_start = (uint8_t*)0x20000;  /* Начинаем с 128KB */
    heap_current = heap_start;
    heap_end = (uint8_t*)0x9AFFF;    /* Куча до 0x9AFFF */
    heap_total_size = 0x9AFFF - 0x20000 + 1; /* 0x7B000 = 504320 байт = 492.5KB */
    heap_used = 0;
    
    /* Инициализируем список блоков */
    block_list = (mem_block_t*)heap_start;
    block_list->size = 0;
    block_list->used = 0;
    block_list->next = NULL;
    
    /* Выводим информацию */
    char buf[32];
    terminal_writestring("OK (using default 636KB)\n");
    
    terminal_writestring("[MEM] Total: ");
    itoa((uint32_t)(memory_map.total_memory / 1024), buf, 10);
    terminal_writestring(buf);
    terminal_writestring(" KB\n");
    
    terminal_writestring("[MEM] Heap: 0x20000-0x9AFFF (");
    itoa(heap_total_size / 1024, buf, 10);
    terminal_writestring(buf);
    terminal_writestring(" KB)\n");
}

/* Вывод карты памяти (упрощенная версия) */
void memory_print_map(void) {
    terminal_writestring("\nMemory Map (Simplified):\n");
    terminal_writestring("==========================\n");
    
    char buf[32];
    
    for (uint32_t i = 0; i < memory_map.entry_count; i++) {
        e820_entry_t* entry = &memory_map.entries[i];
        
        /* Выводим номер записи */
        itoa(i, buf, 10);
        terminal_writestring("Region ");
        terminal_writestring(buf);
        terminal_writestring(": ");
        
        /* Выводим тип */
        switch (entry->type) {
            case E820_TYPE_AVAILABLE:
                terminal_writestring("Available RAM");
                break;
            case E820_TYPE_RESERVED:
                terminal_writestring("Reserved    ");
                break;
            default:
                terminal_writestring("Unknown     ");
                break;
        }
        
        terminal_writestring(" | Base: 0x");
        
        /* Выводим базовый адрес */
        uint32_t base_low = (uint32_t)entry->base_addr;
        itoa(base_low, buf, 16);
        terminal_writestring(buf);
        
        terminal_writestring(" | Length: ");
        
        /* Выводим длину в KB */
        uint32_t length_kb = (uint32_t)(entry->length / 1024);
        itoa(length_kb, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" KB");
        
        terminal_writestring("\n");
    }
    
    /* Выводим итоговую информацию */
    terminal_writestring("==========================\n");
    
    uint32_t total_kb = (uint32_t)(memory_map.total_memory / 1024);
    uint32_t available_kb = (uint32_t)(memory_map.available_memory / 1024);
    uint32_t used_kb = total_kb - available_kb;
    
    terminal_writestring("Total Memory: ");
    itoa(total_kb, buf, 10);
    terminal_writestring(buf);
    terminal_writestring(" KB\n");
    
    terminal_writestring("Used Memory: ");
    itoa(used_kb, buf, 10);
    terminal_writestring(buf);
    terminal_writestring(" KB (");
    itoa((used_kb * 100) / total_kb, buf, 10);
    terminal_writestring(buf);
    terminal_writestring("%)\n");
    
    terminal_writestring("Available Memory: ");
    itoa(available_kb, buf, 10);
    terminal_writestring(buf);
    terminal_writestring(" KB (");
    itoa((available_kb * 100) / total_kb, buf, 10);
    terminal_writestring(buf);
    terminal_writestring("%)\n");
    
    /* Информация о куче */
    terminal_writestring("\nHeap Information:\n");
    terminal_writestring("  Start: 0x20000\n");
    terminal_writestring("  End: 0x");
    itoa((uint32_t)heap_end, buf, 16);
    terminal_writestring(buf);
    terminal_writestring("\n");
    terminal_writestring("  Total size: ");
    itoa(heap_total_size / 1024, buf, 10);
    terminal_writestring(buf);
    terminal_writestring(" KB\n");
    terminal_writestring("  Used: ");
    itoa(heap_used / 1024, buf, 10);
    terminal_writestring(buf);
    terminal_writestring(" KB (");
    if (heap_total_size > 0) {
        itoa((heap_used * 100) / heap_total_size, buf, 10);
        terminal_writestring(buf);
        terminal_writestring("%)\n");
    } else {
        terminal_writestring("0%)\n");
    }
}

/* Получить общий объем памяти */
uint64_t memory_get_total(void) {
    return memory_map.total_memory;
}

/* Получить доступный объем памяти */
uint64_t memory_get_available(void) {
    /* Вычитаем использованную память в куче */
    return memory_map.available_memory - heap_used;
}

/* Получить использованный объем памяти */
uint32_t memory_get_used(void) {
    /* Системная память + использованная куча */
    uint32_t system_used = (uint32_t)(memory_map.total_memory - memory_map.available_memory);
    return system_used + heap_used;
}

/* Получить указатель на карту памяти */
const memory_map_t* memory_get_map(void) {
    return &memory_map;
}

/* Простая аллокация с отслеживанием */
void* memory_alloc(uint32_t size) {
    /* Проверяем, хватает ли места в куче */
    if (heap_current + size + sizeof(mem_block_t) > heap_end) {
        terminal_writestring("[MEM] Allocation failed: out of memory! Requested: ");
        char buf[16];
        itoa(size, buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" bytes, available: ");
        itoa((uint32_t)(heap_end - heap_current), buf, 10);
        terminal_writestring(buf);
        terminal_writestring(" bytes\n");
        return NULL;
    }
    
    /* Создаем блок */
    mem_block_t* block = (mem_block_t*)heap_current;
    heap_current += sizeof(mem_block_t);
    
    block->size = size;
    block->used = 1;
    block->next = block_list;
    block_list = block;
    
    /* Выделяем память для данных */
    void* data_ptr = (void*)heap_current;
    heap_current += size;
    
    /* Обновляем счетчики */
    heap_used += size + sizeof(mem_block_t);
    
    /* Обновляем available_memory */
    if (heap_used <= memory_map.available_memory) {
        memory_map.available_memory -= size + sizeof(mem_block_t);
    }
    
    return data_ptr;
}

/* Освобождение памяти */
void memory_free(void* ptr) {
    if (ptr == NULL) return;
    
    /* Ищем блок, соответствующий указателю */
    mem_block_t* prev = NULL;
    mem_block_t* current = block_list;
    
    while (current != NULL) {
        /* Вычисляем адрес данных этого блока */
        void* block_data = (void*)((uint8_t*)current + sizeof(mem_block_t));
        
        if (block_data == ptr) {
            /* Нашли блок для освобождения */
            current->used = 0;
            
            /* Обновляем счетчики */
            heap_used -= current->size + sizeof(mem_block_t);
            memory_map.available_memory += current->size + sizeof(mem_block_t);
            
            /* Если это последний блок в куче, можем сдвинуть heap_current назад */
            if ((uint8_t*)block_data + current->size == heap_current) {
                heap_current = (uint8_t*)current;
                
                /* Удаляем блок из списка */
                if (prev == NULL) {
                    block_list = current->next;
                } else {
                    prev->next = current->next;
                }
            }
            
            return;
        }
        
        prev = current;
        current = current->next;
    }
    
    terminal_writestring("[MEM] Warning: Attempt to free unallocated memory!\n");
}

/* Получить статистику кучи */
void memory_get_heap_stats(uint32_t* total, uint32_t* used, uint32_t* free_bytes) {
    if (total) *total = heap_total_size;
    if (used) *used = heap_used;
    if (free_bytes) *free_bytes = heap_total_size - heap_used;
}