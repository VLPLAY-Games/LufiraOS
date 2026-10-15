#pragma once

#include "lib/types.h"

// exFAT — v0.9, фаза 2: редизайн по той же идее, что и FAT (см. разбор в
// fs/fat/fat.h) — не грузить устройство целиком в ОЗУ. ОТЛИЧИЕ от FAT:
// exFAT поддерживает вложенные каталоги произвольной глубины, и VFS-слой
// (exfat_mount.c) этим реально пользуется — exfat_lookup_path()/
// exfat_resolve_parent() ходят по произвольному числу компонент пути (в
// отличие от root-only границы у FAT, см. fat_mount.h) — поэтому "кэшировать
// только корень" тут не вариант. Вместо этого — общий РЕЕСТР кэшей
// каталогов (EXFAT_DIR_CACHE_SLOTS штук, LRU-вытеснение), ключ — first_cluster
// каталога; корень в этом реестре — обычный элемент (ключ = fs->root_cluster),
// никакого особого "вечного" слота под него нет.
//
// Резидентны в ОЗУ целиком: boot-регион + таблица FAT (EXFAT_META_MAX_BYTES)
// и Allocation Bitmap (единственный источник истины о занятости кластеров —
// сам по себе обычный файл в куче, но нужен на КАЖДОЕ выделение/освобождение
// кластера, см. разбор exfat.h-истории — кэшируется отдельно и целиком,
// EXFAT_BITMAP_MAX_BYTES). Содержимое ФАЙЛОВ — то, что реально занимает
// объём устройства, — НИКОГДА не копируется в ОЗУ целиком: exfat_read_file()/
// exfat_write_file() читают/пишут прямо на устройство по месту, кластер за
// кластером.
//
// HANDLE: раньше exfat_handle_t был "смещение байта Stream Extension записи
// в fs->image" — прямая адресация внутри одного большого буфера, покрывавшего
// весь диск. Теперь fs->image не покрывает весь диск (только метаданные), так
// что handle переопределён как ПАРА чисел, упакованная в uint64_t:
//   биты 63-32: first_cluster каталога, который СОДЕРЖИТ эту запись (он же
//               ключ в реестре кэшей каталогов)
//   биты 31-0:  смещение байта Stream Extension ВНУТРИ кэш-копии этого
//               каталога (fs->dir_cache[slot].image)
// "Открытый" handle не хранит сырой указатель — только эти два числа, так
// что он остаётся корректным даже если нужный слот кэша успели вытеснить и
// позже подгрузить заново: ЛЮБОЙ доступ по handle идёт через
// dir_cache_get(dir_cluster, ...) заново, по ключу, а не напрямую по байту
// памяти. См. разбор границ этой схемы у get_meta()/exfat_truncate() в
// exfat.c — повторный dir_cache_get() внутри ОДНОЙ операции (между
// walk_dir(), нашедшим handle, и следующим обращением по нему) почти
// гарантированно попадает в тот же слот (он только что был в LRU-топе), а
// если каталог-держатель записи сам когда-либо окажется NoFatChain (foreign,
// не этим драйвером созданный — см. ограничение ниже), повторная загрузка
// по голому handle (без доступного размера каталога) будет ошибочно считать
// его обычной FAT-цепочкой — тот же класс "сознательного ограничения", что и
// ASCII-only имена ниже, не баг.
//
// Сознательные ограничения (без изменений от предыдущей версии):
// - Имена — только ASCII (code point < 128). exFAT хранит имена в UTF-16,
//   этот кернел работает с char* везде.
// - Запись всегда создаёт цепочки через FAT (NoFatChain не выставляется), но
//   ЧТЕНИЕ уважает NoFatChain существующих файлов (например, созданных
//   Windows).
// - Allocation Bitmap (entry 0x81 в корне) — читается и используется как
//   единственный источник истины о занятости кластеров, а не "угадывание" по
//   FAT-записям.

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

// Верхний предел на резидентную область метаданных (boot-регион + таблица
// FAT) — см. разбор архитектуры выше. С большим запасом покрывает любую
// реалистичную exFAT-флешку (таблица FAT растёт как cluster_count*4 байта —
// для огромного раздела с маленькими кластерами может быть заметной, отсюда
// и явный потолок с отдельным кодом ошибки, а не попытка стерпеть любой
// размер).
#define EXFAT_META_MAX_BYTES (16u * 1024u * 1024u)

// Верхний предел на резидентный кэш Allocation Bitmap. 8MB = до 64M
// кластеров (например, больше 1TB при 32KB/кластер) — щедрый запас.
#define EXFAT_BITMAP_MAX_BYTES (8u * 1024u * 1024u)

// Защитный потолок на размер ОДНОГО кластера (байт) — ограничивает размер
// scratch-буфера и буферов кэша каталогов одним выделением. Спецификация
// exFAT позволяет кластеры вплоть до 32MB, но реально используемые размеры
// (4KB - 1MB) на порядки меньше — 1MB уже огромный запас.
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

// Сбрасывает "грязные" метаданные (fs->image), Allocation Bitmap и все
// "грязные" слоты кэша каталогов на устройство через fs->dev — вызывающий
// (exfat_mount.c) делает это сразу после каждой мутации (REAL-TIME запись,
// тот же принцип, что у fat_mount.h), а не по таймеру/unmount. На практике
// каталоги уже сброшены к моменту вызова (мутаторы делают это сами сразу),
// этот проход по ним — просто подстраховка.
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
