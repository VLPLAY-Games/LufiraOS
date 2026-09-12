#pragma once

// Формат диска LufiraFS — версия 1.
//
// Заголовок сознательно не включает ничего, кроме <stdint.h> — его
// подключают и freestanding-ядро, и хостовый tools/mkfs_lufirafs.c (libc).
// Обе стороны обязаны видеть одинаковые структуры/константы/раскладку —
// расхождение здесь означает "ядро не понимает то, что записал mkfs".
//
// Диск размечается ОДНИМ непрерывным регионом (без разделов) так:
//   блок 0                                   — суперблок
//   [bitmap_start .. +bitmap_blocks)          — битовая карта блоков данных
//                                                (1 бит на КАЖДЫЙ блок диска,
//                                                включая служебные — они
//                                                помечаются занятыми при
//                                                форматировании, чтобы
//                                                аллокатор их не трогал)
//   [inode_table_start .. +inode_table_blocks)— таблица inode (фикс. размер)
//   [data_start .. total_blocks)              — блоки данных файлов/директорий

#include <stdint.h>

// UEFI-прошивка умеет читать файлы ТОЛЬКО с FAT12/16/32 (требование самой
// спецификации) — поэтому первые LUFIRAFS_ESP_SIZE байт диска остаются
// маленьким FAT-разделом (ESP) с /EFI/BOOT/BOOTX64.EFI и /kernel.bin, а всё
// остальное место занимает LufiraFS. fat_loader.c грузит в RAM весь диск с
// LBA 0 одним куском, так что ядру достаточно прибавить это смещение к
// bi->FATImageBase (см. kernel.c). Значение общее для kernel и mkfs —
// расхождение означает, что mkfs отформатирует не тот регион, что читает ядро.
#define LUFIRAFS_ESP_SIZE     (4u * 1024u * 1024u)

#define LUFIRAFS_MAGIC        0x31534C4Fu   // "OLS1" по байтам little-endian
#define LUFIRAFS_VERSION      1u
#define LUFIRAFS_BLOCK_SIZE   4096u
#define LUFIRAFS_INODE_SIZE   64u
#define LUFIRAFS_DIRENT_SIZE  64u
#define LUFIRAFS_MAX_NAME     59            // + завершающий '\0' в 60-м байте
#define LUFIRAFS_DIRECT_BLOCKS 12
#define LUFIRAFS_ROOT_INODE   1u            // inode 0 зарезервирован как "нет inode"

// Фиксированное число inode — независимо от размера региона. Для hobby-ОС
// этого с огромным запасом достаточно (512 файлов/директорий), а
// фиксированный размер убирает целый класс "формула разошлась между
// mkfs и ядром" багов.
#define LUFIRAFS_INODE_COUNT  512u

#define LUFIRAFS_MODE_FREE 0u
#define LUFIRAFS_MODE_FILE 1u
#define LUFIRAFS_MODE_DIR  2u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t bitmap_start;
    uint32_t bitmap_blocks;
    uint32_t inode_table_start;
    uint32_t inode_table_blocks;
    uint32_t inode_count;
    uint32_t data_start;
    uint32_t root_inode;
    uint32_t free_blocks;   // кэш для статистики (df) — не источник истины,
                             // при расхождении просто пересчитывается
    uint32_t free_inodes;
    uint8_t  reserved[LUFIRAFS_BLOCK_SIZE - 52];
} __attribute__((packed)) lufirafs_superblock_t;

// mode + size + links_count + direct[12] + indirect = 4+4+4+48+4 = 64 байта.
typedef struct {
    uint32_t mode;      // LUFIRAFS_MODE_*
    uint32_t size;       // байт (для директорий — тоже байт занятых данных)
    uint32_t links_count; // >=1 пока живой; 0 = свободен
    uint32_t direct[LUFIRAFS_DIRECT_BLOCKS];
    uint32_t indirect;    // 0 = нет косвенного блока
} __attribute__((packed)) lufirafs_inode_t;

// inode==0 — свободный/удалённый слот записи каталога.
typedef struct {
    uint32_t inode;
    char     name[LUFIRAFS_MAX_NAME + 1];
} __attribute__((packed)) lufirafs_dirent_t;

// Индексов на один косвенный блок (block_size / 4 байта на указатель).
#define LUFIRAFS_PTRS_PER_BLOCK (LUFIRAFS_BLOCK_SIZE / 4)

// Максимальный размер файла при 12 прямых + 1 одинарном косвенном блоке.
#define LUFIRAFS_MAX_FILE_SIZE \
    (((uint64_t)(LUFIRAFS_DIRECT_BLOCKS + LUFIRAFS_PTRS_PER_BLOCK)) * LUFIRAFS_BLOCK_SIZE)

// Записей-каталога на блок.
#define LUFIRAFS_DIRENTS_PER_BLOCK (LUFIRAFS_BLOCK_SIZE / LUFIRAFS_DIRENT_SIZE)

// Вычисляет полную раскладку диска по размеру региона (в БЛОКАХ).
// Единая функция для kernel И mkfs — единственный источник истины для
// геометрии, чтобы формулы никогда не могли разойтись между сторонами.
static inline void lufirafs_compute_layout(uint32_t total_blocks, lufirafs_superblock_t *sb) {
    sb->magic = LUFIRAFS_MAGIC;
    sb->version = LUFIRAFS_VERSION;
    sb->block_size = LUFIRAFS_BLOCK_SIZE;
    sb->total_blocks = total_blocks;

    sb->bitmap_start = 1; // блок 0 — суперблок
    uint32_t bits_per_block = LUFIRAFS_BLOCK_SIZE * 8;
    sb->bitmap_blocks = (total_blocks + bits_per_block - 1) / bits_per_block;
    if (sb->bitmap_blocks == 0) sb->bitmap_blocks = 1;

    sb->inode_table_start = sb->bitmap_start + sb->bitmap_blocks;
    uint32_t inode_table_bytes = LUFIRAFS_INODE_COUNT * LUFIRAFS_INODE_SIZE;
    sb->inode_table_blocks = (inode_table_bytes + LUFIRAFS_BLOCK_SIZE - 1) / LUFIRAFS_BLOCK_SIZE;
    sb->inode_count = LUFIRAFS_INODE_COUNT;

    sb->data_start = sb->inode_table_start + sb->inode_table_blocks;
    sb->root_inode = LUFIRAFS_ROOT_INODE;
    sb->free_blocks = 0;  // заполняется вызывающей стороной после разметки
    sb->free_inodes = 0;
}
