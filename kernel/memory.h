/* memory.h - управление памятью для LufiraOS */

#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <stddef.h>

/* Структура записи карты памяти E820 */
typedef struct {
    uint64_t base_addr;      /* Начальный адрес диапазона */
    uint64_t length;         /* Длина диапазона в байтах */
    uint32_t type;           /* Тип диапазона */
    uint32_t extended;       /* Расширенные атрибуты (ACPI 3.0) */
} __attribute__((packed)) e820_entry_t;

/* Типы памяти */
#define E820_TYPE_AVAILABLE      1  /* Доступная (свободная) память */
#define E820_TYPE_RESERVED       2  /* Зарезервированная память */
#define E820_TYPE_ACPI_RECLAIM   3  /* Память ACPI, которую можно использовать */
#define E820_TYPE_ACPI_NVS       4  /* Память ACPI NVS */
#define E820_TYPE_BAD            5  /* Поврежденная память */

/* Максимальное количество записей карты памяти */
#define MAX_E820_ENTRIES 32

/* Структура карты памяти системы */
typedef struct {
    uint32_t entry_count;                    /* Количество записей */
    e820_entry_t entries[MAX_E820_ENTRIES];  /* Сами записи */
    uint64_t total_memory;                   /* Общая доступная память */
    uint64_t available_memory;               /* Доступная для использования память */
} memory_map_t;

/* Функции */
void memory_detect(void);
void memory_print_map(void);
uint64_t memory_get_total(void);
uint64_t memory_get_available(void);
uint32_t memory_get_used(void);
const memory_map_t* memory_get_map(void);

/* Простые функции управления памятью */
void* memory_alloc(uint32_t size);
void memory_free(void* ptr);

/* Получить статистику кучи */
void memory_get_heap_stats(uint32_t* total, uint32_t* used, uint32_t* free_bytes);

#endif /* MEMORY_H */