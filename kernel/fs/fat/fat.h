#pragma once

#include "lib/types.h"

// v0.9 — драйвер больше не требует образ ФС целиком в ОЗУ (см. разбор у
// fat_fs_t ниже). Единственный транспорт сегодня — USB-флешки через xHCI
// (fat_mount.c), но сам fat.c про xHCI ничего не знает: он работает через
// эту маленькую абстракцию блочного устройства, как disk.c — через свой
// диспетчер для ATA/AHCI. read_blocks()/write_blocks() всегда оперируют
// 512-байтными блоками (этот драйвер, как и раньше, не поддерживает иной
// размер сектора — проверяется в fat_init()).
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

// Максимальная длина кэшируемой цепочки кластеров корневого каталога
// (только FAT32 — см. комментарий про root_image ниже). 128 кластеров —
// щедрый запас для любого реального корня (даже по 4KB/кластер это 512KB
// одних только записей каталога, т.е. тысячи файлов); превышение означает
// подозрительно огромный или битый корень — mount лучше отклонить (см.
// fat_init()), чем бесконтрольно жрать кучу ядра.
#define FAT_ROOT_CHAIN_MAX 128

// Верхний предел на область метаданных (reserved + таблицы FAT + фикс.
// корень для FAT12/16), которую этот драйвер держит в ОЗУ целиком — см.
// разбор у fat_fs_t. 8MB с огромным запасом покрывает любую реалистичную
// FAT12/16/32 флешку: само содержимое файлов сюда не входит.
#define FAT_META_MAX_SECTORS (16u * 1024u)

typedef struct {
    fat_bpb_t      bpb;
    fat_blockdev_t dev;

    // === Резидентная в ОЗУ область: ТОЛЬКО метаданные ===
    // reserved-секторы + все копии таблицы FAT + (для FAT12/16) фиксированный
    // корневой каталог — то есть всё, что лежит ДО data_start. Это и есть
    // "не монтировать флешку целиком в ОЗУ" (v0.9): раньше fat_init() требовал
    // образ устройства ЦЕЛИКОМ в одном kmalloc'е (и значит, флешку больше
    // 8MB — половины кучи ядра — физически нельзя было смонтировать).
    // Теперь в ОЗУ лежат только метаданные (на практике — десятки-сотни KB
    // даже для гигабайтных флешек), а содержимое ФАЙЛОВ читается/пишется
    // прямо на устройство по месту при каждом fat_read_file()/
    // fat_write_file()/fat_append_file() — см. их комментарии в fat.c.
    uint8_t*    image;
    uint32_t    image_sectors;    /* = data_start; размер fs->image в секторах */

    // === FAT32-only: кэш корневого каталога ===
    // В FAT32 корень — обычная кластерная цепочка В data-регионе, а не
    // фиксированная область, так что технически он НЕ метаданные. Но
    // согласно принятой границе (fat_mount.h) этот драйвер обходит ТОЛЬКО
    // корень (подкаталоги не читаются/не пишутся через VFS), и сам корень
    // почти всегда крошечный (единицы кластеров) — поэтому его дешевле и
    // надёжнее держать резидентно, как и остальные метаданные, чем гонять
    // его через тот же "по месту" путь, что и содержимое файлов. Кэш
    // читается целиком в fat_init() и сбрасывается целиком на устройство
    // после любой мутации (fat_mkdir/fat_rm/fat_create_file/
    // fat_write_file/fat_append_file) — см. root_chain_flush() в fat.c.
    uint8_t*    root_image;
    uint32_t    root_chain[FAT_ROOT_CHAIN_MAX];
    uint32_t    root_chain_len;

    // Разовый буфер размера "один кластер" — для всех операций с кластерами
    // ДАННЫХ (содержимое файлов) и не-корневыми каталогами (инициализация
    // новой под-папки, проверка пустоты удаляемой под-папки): читается/
    // пишется прямо на устройство, НИКОГДА не хранится персистентно между
    // вызовами. См. read_cluster_raw()/write_cluster_raw() в fat.c.
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

// Коды ошибок (кроме 0 = успех) — вызывающие (fat_mount.c, и мёртвый, но
// компилируемый kernel/shell/commands/mount.c) транслируют их в свои
// собственные коды, которые в итоге доходят до userspace mount.elf (см.
// его switch по -2..-9 и komментарий у sys_mount() в syscall.c):
//   -1  не FAT / битый BPB (сигнатура, нулевой делитель, не 512б/сектор)
//   -2  BPB не согласован с реальным устройством (заявляет больше секторов,
//       чем есть, либо метаданные "съели" весь диск, либо кластер нереально большой)
//   -3  метаданные ФС не укладываются в RAM-потолок (FAT_META_MAX_SECTORS)
//   -4  не хватило памяти в куче ядра (kmalloc)
//   -5  ошибка чтения с устройства (метаданные или цепочка корня FAT32)
// На вызывающем — различать "это не FAT" (-1/-2, пробовать ext2/exFAT
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

// Сбрасывает "грязные" метаданные (fs->image) на устройство через
// fs->dev — вызывающий (fat_mount.c) делает это сразу после каждой
// мутации (REAL-TIME запись, см. fat_mount.h), а не по таймеру/unmount.
void fat_sync(fat_fs_t *fs);
