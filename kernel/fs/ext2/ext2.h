#pragma once

#include "lib/types.h"

// ext2 — v0.9, фаза 1, пункт 4. Формат-логика (этот файл) не знает ни про
// USB, ни про VFS — та же архитектура, что fat.h/fat.c (образ целиком в
// RAM, прямая работа по указателям) + ext2_mount.c сверху (VFS/USB-клей,
// как fat_mount.c). Поддержан классический ext2: прямые+одинарно+двойно
// косвенные блоки (без тройной косвенности — для обычных файлов с запасом
// хватает), линейные directory entries, битовые карты блоков/инодов.
// НЕ поддержано (осознанно, не баг): extents (EXT4_FEATURE_INCOMPAT_EXTENTS)
// и 64BIT — ext2_init() отказывается монтировать такую ФС, а не читает её
// неправильно; журнал ext3/ext4 не воспроизводится (как незакоммиченные
// транзакции, так и HAS_JOURNAL сам по себе — не ошибка, просто не трогаем
// журнал, читаем/пишем данные напрямую, тем же способом, что настоящий ext2).

#define EXT2_MAGIC          0xEF53
#define EXT2_ROOT_INO       2
#define EXT2_NAME_LEN       255

#define EXT2_S_IFMT   0xF000
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFDIR  0x4000

#define EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002
#define EXT2_FEATURE_INCOMPAT_EXTENTS  0x0040
#define EXT2_FEATURE_INCOMPAT_64BIT    0x0080

typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    uint8_t  s_unused[820];
} __attribute__((packed)) ext2_superblock_t;

typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed)) ext2_group_desc_t;

typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_size_high;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed)) ext2_inode_t;

typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    // name[name_len] следует сразу за структурой, без терминатора
} __attribute__((packed)) ext2_dirent_t;

typedef struct {
    uint8_t  *image;
    uint32_t  image_size;
    ext2_superblock_t *sb;
    ext2_group_desc_t *groups;
    uint32_t  block_size;
    uint32_t  groups_count;
    uint32_t  inode_size;      // s_inode_size, если DYNAMIC_REV, иначе 128
    int       has_filetype;
    uint8_t  *dirty_map;       // 1 бит/512-байтный сектор образа — см. fat.h
    uint32_t  dirty_map_size;
} ext2_fs_t;

// 0 при успехе (валидный magic, без EXTENTS/64BIT), иначе -1.
int ext2_init(ext2_fs_t *fs, uint8_t *image, uint32_t image_size);

// Резолвит абсолютный путь (без ведущего префикса монтирования — это уже
// снял вызывающий, ext2_mount.c) от корня (inode 2). 0 при успехе.
int ext2_lookup_path(ext2_fs_t *fs, const char *path, uint32_t *out_ino);
int ext2_resolve_parent(ext2_fs_t *fs, const char *path, uint32_t *out_parent_ino, char *leaf_out, int leaf_cap);

int ext2_read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out);

// offset/count в байтах файла; возвращает число реально прочитанных байт
// (0 на offset >= i_size), -1 при структурной ошибке (например, нужен
// неподдержанный тройной косвенный блок).
int ext2_read_file(ext2_fs_t *fs, uint32_t ino, uint32_t offset, void *buf, uint32_t count);

// Пишет, расширяя файл и выделяя блоки по требованию; обновляет i_size.
// Возвращает число записанных байт или -1.
int ext2_write_file(ext2_fs_t *fs, uint32_t ino, uint32_t offset, const void *buf, uint32_t count);

// Освобождает ВСЕ блоки файла и обнуляет i_size (O_TRUNC) — только "до
// нуля", частичное усечение не нужно ни одному вызывающему в этом ядре.
void ext2_truncate(ext2_fs_t *fs, uint32_t ino);

// index — номер записи в каталоге (0, 1, 2, ...), как ожидает vfs_dirent-
// style readdir(..., index). name_out — буфер >= EXT2_NAME_LEN+1.
// Возвращает 1 (есть запись), 0 (конец каталога), -1 (ошибка).
int ext2_readdir(ext2_fs_t *fs, uint32_t dir_ino, int index, char *name_out, uint32_t *ino_out, int *is_dir_out);

// Создаёт обычный файл/каталог с именем name в директории parent_ino.
// mkdir сразу заводит "."/".." и bg_used_dirs_count++. 0 при успехе.
int ext2_create(ext2_fs_t *fs, uint32_t parent_ino, const char *name, uint32_t *out_ino);
int ext2_mkdir(ext2_fs_t *fs, uint32_t parent_ino, const char *name, uint32_t *out_ino);

// Удаляет запись name из parent_ino и освобождает инод/блоки, если это была
// последняя ссылка (unlink) — для каталога требует, чтобы он был пуст
// (только "."/".."). 0 при успехе.
int ext2_unlink(ext2_fs_t *fs, uint32_t parent_ino, const char *name);

// true/false для текущего inode->i_mode.
static inline int ext2_is_dir(const ext2_inode_t *inode) {
    return (inode->i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
}
