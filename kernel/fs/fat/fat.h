#pragma once

#include "lib/types.h"

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

typedef struct {
    fat_bpb_t   bpb;
    uint8_t*    image;
    uint32_t    fat_start;
    uint32_t    root_dir_start;
    uint32_t    data_start;
    uint32_t    root_entries;
    uint32_t    cluster_size;
    uint32_t    total_sectors;
    uint8_t     fat_type;
    uint32_t    root_cluster;
    uint32_t    sectors_per_fat;   /* реальное количество секторов на FAT */
    uint32_t    max_cluster;       /* максимальный допустимый номер кластера + 1 */
    /* dirty‑карта */
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

int fat_init(fat_fs_t *fs, void *image, uint32_t image_size);
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
void fat_flush(fat_fs_t *fs);

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