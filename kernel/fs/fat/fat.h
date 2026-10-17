#pragma once

#include "lib/types.h"

// Драйвер не требует образ ФС целиком в ОЗУ (см. fat_fs_t ниже). Транспорт —
// USB-флешки через xHCI (fat_mount.c), но fat.c про xHCI не знает: работает
// через эту абстракцию блочного устройства, как disk.c — через свой
// диспетчер ATA/AHCI. read_blocks()/write_blocks() всегда оперируют
// 512-байтными блоками (иной размер сектора не поддерживается, проверяется
// в fat_init()).
typedef struct {
    void *ctx;
    int (*read_blocks)(void *ctx, uint32_t lba, uint32_t count, void *buf);
    int (*write_blocks)(void *ctx, uint32_t lba, uint32_t count, const void *buf);
} fat_blockdev_t;

typedef struct {
    uint8_t  jump[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
    /* FAT32 */
    uint32_t sectors_per_fat_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved2[12];
} __attribute__((packed)) fat_bpb_t;

typedef struct {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  reserved;
    uint8_t  creation_time_tenths;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high;
    uint16_t modification_time;
    uint16_t modification_date;
    uint16_t first_cluster_low;
    uint32_t file_size;
} __attribute__((packed)) fat_dir_entry_t;

// Максимальная длина кэшируемой цепочки кластеров корня (только FAT32, см.
// root_image ниже). 128 кластеров — тысячи записей каталога; превышение
// означает битый/подозрительный корень, mount лучше отклонить, чем
// бесконтрольно жрать кучу ядра.
#define FAT_ROOT_CHAIN_MAX 128

// Верхний предел на область метаданных (reserved + таблицы FAT + фикс.
// корень для FAT12/16), которую драйвер держит в ОЗУ целиком (см.
// fat_fs_t). 8MB с запасом покрывает любую реальную флешку; содержимое
// файлов сюда не входит.
#define FAT_META_MAX_SECTORS (16u * 1024u)

typedef struct {
    fat_bpb_t      bpb;
    fat_blockdev_t dev;

    // === Резидентная в ОЗУ область: только метаданные ===
    // reserved-секторы + все копии таблицы FAT + (для FAT12/16) фиксированный
    // корневой каталог — всё, что лежит до data_start. Содержимое файлов в
    // ОЗУ не держится — читается/пишется прямо на устройство по месту при
    // каждом fat_read_file()/fat_write_file()/fat_append_file().
    uint8_t*    image;
    uint32_t    image_sectors;    /* = data_start; размер fs->image в секторах */

    // === FAT32-only: кэш корневого каталога ===
    // В FAT32 корень — кластерная цепочка в data-регионе, а не фиксированная
    // область, так что формально это не метаданные. Но драйвер обходит
    // только корень (подкаталоги не читаются/не пишутся через VFS), а сам
    // он почти всегда крошечный — поэтому держится резидентно, как и
    // остальные метаданные. Читается целиком в fat_init(), сбрасывается
    // целиком после любой мутации (root_chain_flush() в fat.c).
    uint8_t*    root_image;
    uint32_t    root_chain[FAT_ROOT_CHAIN_MAX];
    uint32_t    root_chain_len;

    // Разовый буфер "один кластер" — для операций с кластерами ДАННЫХ и
    // не-корневыми каталогами; читается/пишется прямо на устройство,
    // никогда не хранится между вызовами (read_cluster_raw()/
    // write_cluster_raw() в fat.c).
    uint8_t*    scratch_cluster;

    uint32_t    fat_start;
    uint32_t    root_dir_start;
    uint32_t    data_start;
    uint32_t    root_entries;
    uint32_t    cluster_size;
    uint32_t    total_sectors;    /* ВСЕГО на устройстве, не только резидентная часть */
    uint8_t     fat_type;
    uint32_t    root_cluster;
    uint32_t    sectors_per_fat;   /* реальное количество секторов на FAT */
    uint32_t    max_cluster;       /* максимальный допустимый номер кластера + 1 */
    /* dirty‑карта — покрывает ТОЛЬКО fs->image (image_sectors), не весь диск */
    uint8_t*    dirty_map;
    uint32_t    dirty_map_size;
} fat_fs_t;

typedef struct {
    fat_fs_t*   fs;
    uint32_t    first_cluster;
    uint32_t    current_cluster;
    uint32_t    entries_left_in_dir;
    uint32_t    entry_index_in_cluster;
    uint32_t    total_entries;
    uint32_t    entries_per_cluster;
    int         is_root;
} fat_dir_t;

/* === Публичный API === */
int fat_find_entry(fat_fs_t *fs, uint32_t dir_cluster,
                   const char *filename, fat_dir_entry_t *out_entry);

uint16_t read_le16(const uint8_t *p);
uint32_t read_le32(const uint8_t *p);

// Коды ошибок (кроме 0 = успех) — транслируются вызывающими (fat_mount.c
// и kernel/shell/commands/mount.c) в коды userspace mount.elf:
//   -1  не FAT / битый BPB (сигнатура, нулевой делитель, не 512б/сектор)
//   -2  BPB не согласован с устройством (секторов больше, чем есть,
//       метаданные "съели" весь диск, или кластер нереально большой)
//   -3  метаданные не укладываются в RAM-потолок (FAT_META_MAX_SECTORS)
//   -4  не хватило памяти в куче ядра (kmalloc)
//   -5  ошибка чтения с устройства (метаданные или цепочка корня FAT32)
// Вызывающему важно различать "это не FAT" (-1/-2, пробовать ext2/exFAT
// дальше) от "это FAT, но не влезает/не читается" (-3/-4/-5).
int fat_init(fat_fs_t *fs, const fat_blockdev_t *dev, uint32_t total_sectors);
void fat_deinit(fat_fs_t *fs);
int fat_open(fat_fs_t *fs, const char *filename, uint32_t *size);
int fat_read_file(fat_fs_t *fs, const char *filename, void *buffer, uint32_t size);
int fat_list_root(fat_fs_t *fs, char names[][12], int max_count);

int fat_opendir(fat_fs_t *fs, uint32_t first_cluster, fat_dir_t *dir);
int fat_readdir(fat_dir_t *dir, fat_dir_entry_t *entry);
int fat_closedir(fat_dir_t *dir);
int fat_mkdir(fat_fs_t *fs, uint32_t parent_cluster, const char *name);
int fat_rm(fat_fs_t *fs, uint32_t parent_cluster, const char *name);
int fat_create_file(fat_fs_t *fs, uint32_t parent_cluster, const char *name);

void fat_mark_sector_dirty(fat_fs_t *fs, uint32_t lba);

int fat_write_file(fat_fs_t *fs, const char *filename, const void *buffer, uint32_t size);
int fat_append_file(fat_fs_t *fs, const char *filename, const void *buffer, uint32_t size);
int fat_truncate_file(fat_fs_t *fs, const char *filename, uint32_t new_size);
int fat_rename_file(fat_fs_t *fs, const char *old_name, const char *new_name);

/* === Внутренние функции, экспортированные для VFS === */
uint32_t get_fat_entry(fat_fs_t *fs, uint32_t cluster);
int is_eoc(fat_fs_t *fs, uint32_t cluster);
void to_short_name(const char *filename, char out[11]);

int fat_lookup_path(fat_fs_t *fs,
                    const char *path,
                    fat_dir_entry_t *out_entry,
                    uint32_t *out_cluster);

int fat_resolve_parent(fat_fs_t *fs,
                       const char *path,
                       uint32_t *out_parent_cluster,
                       char *out_name);

// Сбрасывает "грязные" метаданные (fs->image) на устройство через fs->dev —
// вызывающий (fat_mount.c) делает это сразу после каждой мутации, не по
// таймеру/unmount (см. REAL-TIME в fat_mount.h).
void fat_sync(fat_fs_t *fs);
