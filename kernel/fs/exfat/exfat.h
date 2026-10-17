#pragma once

#include "lib/types.h"

// exFAT: та же идея, что и FAT (fs/fat/fat.h) — не грузить устройство целиком
// в ОЗУ. Отличие от FAT: exFAT поддерживает вложенные каталоги произвольной
// глубины, exfat_lookup_path()/exfat_resolve_parent() ходят по произвольному
// числу компонент пути (в отличие от root-only границы FAT, см. fat_mount.h),
// так что "кэшировать только корень" не вариант — вместо этого общий реестр
// кэшей каталогов (EXFAT_DIR_CACHE_SLOTS, LRU-вытеснение), ключ —
// first_cluster каталога; корень в реестре обычный элемент (ключ =
// fs->root_cluster), без особого "вечного" слота.
//
// Резидентны в ОЗУ целиком: boot-регион + таблица FAT (EXFAT_META_MAX_BYTES)
// и Allocation Bitmap — единственный источник истины о занятости кластеров,
// нужен на каждое выделение/освобождение, кэшируется отдельно целиком
// (EXFAT_BITMAP_MAX_BYTES). Содержимое файлов никогда не копируется в ОЗУ
// целиком: exfat_read_file()/exfat_write_file() читают/пишут прямо на
// устройство по месту, кластер за кластером.
//
// HANDLE — пара чисел, упакованная в uint64_t (не сырой указатель, т.к.
// fs->image не покрывает весь диск и слот кэша каталога может быть вытеснен):
//   биты 63-32: first_cluster каталога, содержащего запись (ключ кэша)
//   биты 31-0:  смещение байта Stream Extension внутри кэш-копии каталога
//               (fs->dir_cache[slot].image)
// Любой доступ по handle идёт через dir_cache_get(dir_cluster, ...) заново,
// по ключу — так handle остаётся корректным после вытеснения и перезагрузки
// слота. Если каталог-держатель записи окажется NoFatChain (не этим
// драйвером созданный), повторная загрузка по голому handle ошибочно сочтёт
// его обычной FAT-цепочкой — тот же класс ограничения, что ASCII-only имена
// ниже, не баг.
//
// Сознательные ограничения:
// - Имена — только ASCII (code point < 128); exFAT хранит их в UTF-16, этот
//   кернел работает с char*.
// - Запись всегда создаёт цепочки через FAT (NoFatChain не выставляется), но
//   чтение уважает NoFatChain существующих файлов (например, от Windows).
// - Allocation Bitmap (entry 0x81 в корне) — единственный источник истины о
//   занятости кластеров, а не "угадывание" по FAT-записям.

#define EXFAT_CLUSTER_FREE 2 // первый валидный номер кластера

// Абстракция блочного устройства — та же идея и та же форма, что
// fat_blockdev_t (fs/fat/fat.h): exfat.c не знает про xHCI напрямую,
// read_blocks()/write_blocks() всегда оперируют 512-байтными блоками (это
// нативный размер блока USB MSD — xhci_msd_get_info() отвергает другие
// размеры ещё в exfat_mount.c, до вызова exfat_init()).
typedef struct {
    void *ctx;
    int (*read_blocks)(void *ctx, uint32_t lba, uint32_t count, void *buf);
    int (*write_blocks)(void *ctx, uint32_t lba, uint32_t count, const void *buf);
} exfat_blockdev_t;

// Верхний предел на резидентные метаданные (boot-регион + таблица FAT).
// Таблица FAT растёт как cluster_count*4 байта — для раздела с маленькими
// кластерами может быть заметной, отсюда явный потолок с кодом ошибки.
#define EXFAT_META_MAX_BYTES (16u * 1024u * 1024u)

// Верхний предел на резидентный кэш Allocation Bitmap. 8MB = до 64M
// кластеров (>1TB при 32KB/кластер) — щедрый запас.
#define EXFAT_BITMAP_MAX_BYTES (8u * 1024u * 1024u)

// Защитный потолок на размер одного кластера — ограничивает scratch-буфер и
// буферы кэша каталогов одним выделением. Спецификация допускает кластеры
// до 32MB, реальные размеры (4KB-1MB) на порядки меньше.
#define EXFAT_CLUSTER_MAX_BYTES (1u * 1024u * 1024u)

// Реестр кэшей каталогов — см. разбор архитектуры вверху файла. Ключ —
// first_cluster каталога (уникален: один каталог не может начинаться с двух
// разных кластеров).
#define EXFAT_DIR_CACHE_SLOTS 8
#define EXFAT_DIR_CACHE_MAX_CLUSTERS 256

typedef struct {
    int       in_use;
    uint32_t  first_cluster;
    int       no_fat_chain;
    uint32_t  chain[EXFAT_DIR_CACHE_MAX_CLUSTERS];
    uint32_t  chain_len;
    uint8_t  *image;        // chain_len * cluster_size байт, в порядке цепочки
    int       dirty;
    uint32_t  lru_tick;
} exfat_dir_cache_t;

typedef struct {
    exfat_blockdev_t dev;

    // === Резидентные метаданные: boot-регион + таблица FAT ===
    uint8_t  *image;
    uint32_t  image_sectors;     // = cluster_heap_offset_bytes / 512
    uint8_t  *dirty_map;         // по image_sectors (только таблица FAT мутирует)
    uint32_t  dirty_map_size;

    // === Резидентный кэш Allocation Bitmap (НЕ указатель в fs->image —
    // отдельное выделение, см. разбор выше) ===
    uint32_t  bitmap_cluster;    // 0, если Allocation Bitmap не найден
    uint8_t  *bitmap;
    uint32_t  bitmap_bits;
    uint32_t  bitmap_chain[EXFAT_DIR_CACHE_MAX_CLUSTERS];
    uint32_t  bitmap_chain_len;

    // === Разовый буфер "один кластер" — для данных файлов (стримятся
    // напрямую на устройство) и не-кэшируемых операций (зануление нового
    // кластера в alloc_cluster()) ===
    uint8_t  *scratch_cluster;

    // === Реестр кэшей каталогов ===
    exfat_dir_cache_t dir_cache[EXFAT_DIR_CACHE_SLOTS];
    uint32_t dir_cache_clock;

    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t fat_offset_bytes;
    uint32_t fat_length_bytes;
    uint32_t cluster_heap_offset_bytes;
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint32_t total_sectors;      // ВСЕГО на устройстве, не только резидентная часть
} exfat_fs_t;

// Идентифицирует файл/каталог — см. подробный разбор вверху файла.
// EXFAT_ROOT_HANDLE — сентинел "это сам корень", не запись внутри
// какого-то каталога (у корня нет Stream Extension вообще — он задан прямо
// в boot-секторе).
typedef uint64_t exfat_handle_t;
#define EXFAT_ROOT_HANDLE 0xFFFFFFFFFFFFFFFFull

// Коды ошибок exfat_init() (кроме 0 = успех) — тот же контракт, что у
// fat_init() (fs/fat/fat.h), вызывающий (exfat_mount.c) транслирует их в
// свои коды тем же способом:
//   -1  не exFAT / битый boot-сектор (сигнатура/размер сектора/нулевой делитель)
//   -2  поля BPB не согласованы с реальным устройством (вылезают за его
//       пределы, либо таблица FAT не укладывается в заявленные метаданные)
//   -3  метаданные (boot+FAT) или Allocation Bitmap не укладываются в
//       RAM-потолок (EXFAT_META_MAX_BYTES / EXFAT_BITMAP_MAX_BYTES)
//   -4  не хватило памяти в куче ядра
//   -5  ошибка чтения с устройства, либо не найден/не читается Allocation Bitmap
int exfat_init(exfat_fs_t *fs, const exfat_blockdev_t *dev, uint32_t total_sectors);
void exfat_deinit(exfat_fs_t *fs);

// Сбрасывает "грязные" метаданные, Allocation Bitmap и грязные слоты кэша
// каталогов на устройство — вызывающий (exfat_mount.c) делает это сразу
// после каждой мутации (REAL-TIME, как в fat_mount.h). Каталоги мутаторы
// обычно сбрасывают сами сразу, этот проход — подстраховка.
void exfat_sync(exfat_fs_t *fs);

int exfat_lookup_path(exfat_fs_t *fs, const char *path, exfat_handle_t *out);
int exfat_resolve_parent(exfat_fs_t *fs, const char *path, exfat_handle_t *out_parent, char *leaf_out, int leaf_cap);

int exfat_is_dir(exfat_fs_t *fs, exfat_handle_t h);
uint64_t exfat_size(exfat_fs_t *fs, exfat_handle_t h);

int exfat_read_file(exfat_fs_t *fs, exfat_handle_t h, uint64_t offset, void *buf, uint32_t count);
int exfat_write_file(exfat_fs_t *fs, exfat_handle_t h, uint64_t offset, const void *buf, uint32_t count);
void exfat_truncate(exfat_fs_t *fs, exfat_handle_t h);

// index — как у ext2_readdir()/обычного vfs readdir: 0,1,2,...
int exfat_readdir(exfat_fs_t *fs, exfat_handle_t dir, int index, char *name_out, exfat_handle_t *out, int *is_dir_out);

int exfat_create(exfat_fs_t *fs, exfat_handle_t parent, const char *name, int is_dir, exfat_handle_t *out);
int exfat_unlink(exfat_fs_t *fs, exfat_handle_t parent, const char *name);
