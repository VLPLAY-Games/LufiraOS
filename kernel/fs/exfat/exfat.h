#pragma once

#include "lib/types.h"

// exFAT — v0.9, фаза 1, пункт 4 (вторая половина). Та же архитектура, что
// ext2.h/ext2.c: образ целиком в RAM, прямая работа по указателям,
// отдельный *_mount.c сверху даёт VFS/USB-клей. Поддержаны вложенные
// каталоги произвольной глубины (формат это позволяет нативно — как
// FAT12/16/32, только с другой раскладкой directory entry).
//
// Сознательные ограничения (не баги):
// - Имена — только ASCII (code point < 128). exFAT хранит имена в UTF-16,
//   этот кернел работает с char* везде — полный Unicode здесь так же не
//   нужен, как длинные имена не нужны были классическому FAT-драйверу
//   (fat.c, 8.3). NameHash/верхний регистр считаются по ASCII-правилам
//   (toupper), что corresponds дефолтной up-case таблице спецификации
//   ровно на этом диапазоне — настоящая Up-case Table (entry 0x82 в корне)
//   не читается и не нужна для корректности в пределах ASCII.
// - Запись всегда создаёт цепочки через FAT (NoFatChain не выставляется),
//   но ЧТЕНИЕ уважает NoFatChain существующих файлов (например, созданных
//   Windows) — иначе чтение было бы в принципе неверным, не просто неполным.
// - Allocation Bitmap (entry 0x81 в корне) — читается и используется как
//   единственный источник истины о занятости кластеров (это то, что реально
//   проверяют сторонние chkdsk/fsck), а не "угадывание" по FAT-записям.

#define EXFAT_CLUSTER_FREE 2 // первый валидный номер кластера

typedef struct {
    uint8_t *image;
    uint32_t image_size;

    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t fat_offset_bytes;
    uint32_t fat_length_bytes;
    uint32_t cluster_heap_offset_bytes;
    uint32_t cluster_count;
    uint32_t root_cluster;

    uint32_t bitmap_cluster;   // 0, если Allocation Bitmap не найден (без записи)
    uint8_t *bitmap;           // прямой указатель в image на сам битмап
    uint32_t bitmap_bits;

    uint8_t *dirty_map;
    uint32_t dirty_map_size;
} exfat_fs_t;

// Идентифицирует файл/каталог: смещение (в байтах от начала image) Stream
// Extension (0xC0) записи этого объекта в его родительском каталоге —
// Primary (0x85) запись всегда ровно на 32 байта раньше. Для КОРНЯ
// каталога — специальный сентинел, у которого нет Stream Extension вообще
// (корень задан прямо в boot-секторе).
#define EXFAT_ROOT_HANDLE 0xFFFFFFFFu
typedef uint32_t exfat_handle_t;

int exfat_init(exfat_fs_t *fs, uint8_t *image, uint32_t image_size);

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
