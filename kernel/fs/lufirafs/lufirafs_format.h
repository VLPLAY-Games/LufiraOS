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

// Размер партиции 1 (ESP, FAT12 — /EFI/BOOT/BOOTX64.EFI и /kernel.bin) в
// MBR-разметке диска (v1.0.0 план — см. LufiraOS-Builder/lufira_builder/
// config.py, ESP_SIZE и _write_mbr()). Ядро саму ESP не трогает (её не
// монтирует и не читает — LufiraFS находит себя по MBR-партиции, см.
// drivers/disk/mbr.c), константа здесь чисто для документации/проверки
// совпадения значений между kernel и mkfs при расхождении формата.
#define LUFIRAFS_ESP_SIZE     (4u * 1024u * 1024u)

#define LUFIRAFS_MAGIC        0x31534C4Fu   // "OLS1" по байтам little-endian
#define LUFIRAFS_VERSION      3u            // v3: block_size 4096 -> 8192 (см. ниже)
// Был 4096 — при 12 прямых + 1 косвенном блоке (1024 указателя на блок)
// максимальный размер файла получался ~4.05МиБ (LUFIRAFS_MAX_FILE_SIZE
// ниже), а doom.lpg — уже 4.77МиБ: любая запись после ~4.05МиБ упиралась
// в проверку в lufirafs_write() и возвращала -1 ("write error while
// staging download" в dlpg.c). Тот же косвенный блок при block_size=8192
// даёт 2048 указателей — максимум файла ~16.09МиБ, с запасом. Несовместимо
// со старыми образами диска (lufirafs_init() и так сверяет sb.block_size
// с этой константой и отказывает при расхождении) — ожидается полная
// пересборка через LufiraOS-Builder, отдельных пользовательских дисков
// с данными пока не существует.
#define LUFIRAFS_BLOCK_SIZE   8192u
#define LUFIRAFS_INODE_SIZE   76u
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

// mode + size + links_count + direct[12] + indirect + uid + gid + perm
// = 4+4+4+48+4+4+4+4 = 76 байт.
typedef struct {
    uint32_t mode;      // LUFIRAFS_MODE_*
    uint32_t size;       // байт (для директорий — тоже байт занятых данных)
    uint32_t links_count; // >=1 пока живой; 0 = свободен
    uint32_t direct[LUFIRAFS_DIRECT_BLOCKS];
    uint32_t indirect;    // 0 = нет косвенного блока
    uint32_t uid;         // владелец (0 = root)
    uint32_t gid;         // группа-владелец
    uint32_t perm;        // классические 9 бит rwxrwxrwx (напр. 0644/0755)
} __attribute__((packed)) lufirafs_inode_t;

#define LUFIRAFS_DEFAULT_FILE_PERM 0644u
#define LUFIRAFS_DEFAULT_DIR_PERM  0755u

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
