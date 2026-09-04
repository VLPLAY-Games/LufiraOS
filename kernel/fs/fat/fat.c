#include "fat.h"
#include "drivers/disk/disk.h"
#include "system/mm/heap.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"

static void* memcpy(void* dest, const void* src, unsigned int n) {
    char* d = (char*)dest;
    const char* s = (const char*)src;
    while (n--) *d++ = *s++;
    return dest;
}

static void* memset(void* s, int c, unsigned int n) {
    unsigned char* p = (unsigned char*)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

uint32_t read_le32(const uint8_t *p) {
    return p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24);
}

uint16_t read_le16(const uint8_t *p) {
    return p[0] | (p[1]<<8);
}

static void write_le16(uint8_t *p, uint16_t val) {
    p[0] = val & 0xFF;
    p[1] = (val >> 8) & 0xFF;
}

static void write_le32(uint8_t *p, uint32_t val) {
    p[0] = val & 0xFF;
    p[1] = (val >> 8) & 0xFF;
    p[2] = (val >> 16) & 0xFF;
    p[3] = (val >> 24) & 0xFF;
}

static const uint8_t* cluster_to_sector(fat_fs_t *fs, uint32_t cluster) {
    if (cluster < 2 || cluster >= fs->max_cluster) return 0;
    uint32_t lba = fs->data_start + (cluster - 2) * fs->cluster_size;
    return fs->image + lba * 512;
}

/* ======== Dirty bitmap helpers ======== */
void fat_mark_sector_dirty(fat_fs_t *fs, uint32_t lba) {
    if (lba >= fs->total_sectors) return;
    fs->dirty_map[lba >> 3] |= (1 << (lba & 7));
}

static void mark_cluster_dirty(fat_fs_t *fs, uint32_t cluster) {
    if (cluster < 2) return;
    uint32_t lba = fs->data_start + (cluster - 2) * fs->cluster_size;
    for (uint32_t i = 0; i < fs->cluster_size; i++)
        fat_mark_sector_dirty(fs, lba + i);
}

static void mark_fat_sector_dirty(fat_fs_t *fs, uint32_t cluster) {
    uint32_t fat_offset;
    switch (fs->fat_type) {
        case 12: fat_offset = cluster + cluster / 2; break;
        case 16: fat_offset = cluster * 2; break;
        case 32: fat_offset = cluster * 4; break;
        default: return;
    }
    uint32_t lba = fs->fat_start + fat_offset / 512;
    fat_mark_sector_dirty(fs, lba);
    if (fat_offset % 512 > 508)
        fat_mark_sector_dirty(fs, lba + 1);
}

/* ======== Инициализация FAT с выделением dirty‑map ======== */

int fat_init(fat_fs_t *fs, void *image, uint32_t image_size) {
    if (!image || image_size < 512) return -1;
    fs->image = (uint8_t*)image;
    const uint8_t *raw = (const uint8_t*)image;
    fs->bpb.bytes_per_sector = read_le16(raw+11);
    fs->bpb.sectors_per_cluster = raw[13];
    fs->bpb.reserved_sectors = read_le16(raw+14);
    fs->bpb.num_fats = raw[16];
    fs->bpb.root_entries = read_le16(raw+17);
    fs->bpb.total_sectors_16 = read_le16(raw+19);
    fs->bpb.total_sectors_32 = read_le32(raw+32);
    fs->bpb.sectors_per_fat_16 = read_le16(raw+22);
    fs->bpb.media = raw[21];

    uint16_t bytes_per_sec = fs->bpb.bytes_per_sector;
    uint8_t sec_per_cl = fs->bpb.sectors_per_cluster;
    uint16_t reserved = fs->bpb.reserved_sectors;
    uint8_t fats = fs->bpb.num_fats;
    uint32_t total_sec = fs->bpb.total_sectors_16 ?
                         fs->bpb.total_sectors_16 : fs->bpb.total_sectors_32;

    if (total_sec * bytes_per_sec > image_size) return -2;

    uint32_t sectors_per_fat = fs->bpb.sectors_per_fat_16;
    if (sectors_per_fat == 0) {
        sectors_per_fat = read_le32(raw+36); // FAT32
    }

    uint32_t fat_start = reserved;
    uint32_t root_dir_start = fat_start + fats * sectors_per_fat;
    uint32_t data_start = root_dir_start +
        (fs->bpb.root_entries * 32 + bytes_per_sec - 1) / bytes_per_sec;

    uint32_t data_sec = total_sec - data_start;
    uint32_t count_of_clusters = data_sec / sec_per_cl;

    uint8_t fat_type = 16;
    if (count_of_clusters < 4085) fat_type = 12;
    else if (count_of_clusters < 65525) fat_type = 16;
    else fat_type = 32;

    fs->fat_start = fat_start;
    fs->root_dir_start = root_dir_start;
    fs->data_start = data_start;
    fs->root_entries = fs->bpb.root_entries;
    fs->cluster_size = sec_per_cl;
    fs->total_sectors = total_sec;
    fs->fat_type = fat_type;
    fs->sectors_per_fat = sectors_per_fat;      /* сохраняем реальное значение */
    fs->max_cluster = count_of_clusters + 2;    /* кластеры от 2 до count_of_clusters+1 */

    if (fat_type == 32) {
        fs->root_cluster = read_le32(raw+44);
    } else {
        fs->root_cluster = 0;
    }

    /* Инициализация dirty‑карты */
    uint32_t map_size = (total_sec + 7) / 8;
    fs->dirty_map = (uint8_t*)kmalloc(map_size);
    if (!fs->dirty_map) return -3;
    fs->dirty_map_size = map_size;
    memset(fs->dirty_map, 0, map_size);

    return 0;
}

/* ======== Работа с таблицей FAT ======== */
uint32_t get_fat_entry(fat_fs_t *fs, uint32_t cluster) {
    if (cluster < 2) return 0xFFF7;
    uint8_t *fat = fs->image + fs->fat_start * 512;
    uint32_t offset, entry;
    switch (fs->fat_type) {
        case 12:
            offset = cluster + (cluster >> 1);
            entry = fat[offset] | (fat[offset+1] << 8);
            if (cluster & 1) entry >>= 4;
            else entry &= 0xFFF;
            if (entry >= 0xFF8) entry |= 0xFFFFF000;
            break;
        case 16:
            offset = cluster * 2;
            entry = fat[offset] | (fat[offset+1] << 8);
            break;
        case 32:
            offset = cluster * 4;
            entry = fat[offset] | (fat[offset+1]<<8) | (fat[offset+2]<<16) | (fat[offset+3]<<24);
            entry &= 0x0FFFFFFF;
            break;
        default:
            return 0xFFF7;
    }
    return entry;
}

int is_eoc(fat_fs_t *fs, uint32_t cluster) {
    if (fs->fat_type == 12) return cluster >= 0xFF8;
    if (fs->fat_type == 16) return cluster >= 0xFFF8;
    return cluster >= 0x0FFFFFF8;
}

/* Установка записи FAT с пометкой грязных секторов */
static void set_fat_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value) {
    if (cluster < 2) return;

    uint32_t sectors_per_fat = fs->sectors_per_fat;  /* используем сохранённое значение */

    for (uint32_t fat_index = 0; fat_index < fs->bpb.num_fats; fat_index++) {
        uint8_t *fat = fs->image +
            (fs->fat_start + fat_index * sectors_per_fat) * 512;

        uint32_t offset;
        switch (fs->fat_type) {
            case 12:
                offset = cluster + (cluster >> 1);
                if (cluster & 1) {
                    fat[offset] = (fat[offset] & 0x0F) | ((value & 0xF) << 4);
                    fat[offset + 1] = (value >> 4) & 0xFF;
                } else {
                    fat[offset] = value & 0xFF;
                    fat[offset + 1] = (fat[offset + 1] & 0xF0) | ((value >> 8) & 0x0F);
                }
                break;
            case 16:
                offset = cluster * 2;
                fat[offset] = value & 0xFF;
                fat[offset + 1] = (value >> 8) & 0xFF;
                break;
            case 32:
                offset = cluster * 4;
                fat[offset] = value & 0xFF;
                fat[offset + 1] = (value >> 8) & 0xFF;
                fat[offset + 2] = (value >> 16) & 0xFF;
                fat[offset + 3] = (value >> 24) & 0xFF;
                break;
        }
    }
    mark_fat_sector_dirty(fs, cluster);
}

void to_short_name(const char *filename, char out[11]) {
    for (int i=0; i<11; i++) out[i] = ' ';
    int i=0, j=0;
    while (filename[i] && j<8) {
        if (filename[i]=='.') break;
        char c = filename[i];
        if (c>='a' && c<='z') c -= 32;
        out[j++] = c;
        i++;
    }
    while (filename[i] && filename[i]!='.') i++;
    if (filename[i]=='.') {
        i++;
        j=8;
        while (filename[i] && j<11) {
            char c = filename[i];
            if (c>='a' && c<='z') c -= 32;
            out[j++] = c;
            i++;
        }
    }
}

static fat_dir_entry_t* find_entry_in_dir(fat_fs_t *fs, uint32_t dir_cluster,
                                         const char name8_3[11],
                                         int is_root_fixed)
{
    if (is_root_fixed) {
        uint8_t *dir = fs->image + fs->root_dir_start * 512;
        for (uint32_t i = 0; i < fs->root_entries; i++) {
            fat_dir_entry_t *e = (fat_dir_entry_t*)(dir + i*32);
            if (e->name[0] == 0x00) break;
            if (e->name[0] == 0xE5 || e->attr == 0x0F) continue;
            int match = 1;
            for (int j = 0; j < 11; j++)
                if (e->name[j] != (uint8_t)name8_3[j]) { match=0; break; }
            if (match) return e;
        }
    } else {
        uint32_t cluster = dir_cluster;
        while (cluster >= 2) {
            uint8_t *data = (uint8_t*)cluster_to_sector(fs, cluster);
            if (!data) break;
            uint32_t entries_per_cluster = (fs->cluster_size * 512) / 32;
            for (uint32_t i = 0; i < entries_per_cluster; i++) {
                fat_dir_entry_t *e = (fat_dir_entry_t*)(data + i*32);
                if (e->name[0] == 0x00) break;
                if (e->name[0] == 0xE5 || e->attr == 0x0F) continue;
                int match = 1;
                for (int j = 0; j < 11; j++)
                    if (e->name[j] != (uint8_t)name8_3[j]) { match=0; break; }
                if (match) return e;
            }
            cluster = get_fat_entry(fs, cluster);
        }
    }
    return NULL;
}

int fat_find_entry(fat_fs_t *fs, uint32_t dir_cluster,
                   const char *filename, fat_dir_entry_t *out_entry)
{
    char sname[11];
    to_short_name(filename, sname);
    fat_dir_entry_t *e;
    if (dir_cluster == 0 && fs->fat_type != 32) {
        e = find_entry_in_dir(fs, 0, sname, 1);
    } else {
        e = find_entry_in_dir(fs, dir_cluster, sname, 0);
    }
    if (e) {
        memcpy(out_entry, e, sizeof(fat_dir_entry_t));
        return 0;
    }
    return -1;
}

static fat_dir_entry_t* find_in_root(fat_fs_t *fs, const char* name8_3) {
    uint32_t dir_cluster = (fs->fat_type == 32) ? fs->root_cluster : 0;
    int is_root_fixed = (fs->fat_type != 32);
    return find_entry_in_dir(fs, dir_cluster, name8_3, is_root_fixed);
}

int fat_open(fat_fs_t *fs, const char *filename, uint32_t *size) {
    char sname[11];
    to_short_name(filename, sname);
    fat_dir_entry_t *e = find_in_root(fs, sname);
    if (!e) return -1;
    *size = read_le32((const uint8_t*)&e->file_size);
    return 0;
}

int fat_read_file(fat_fs_t *fs, const char *filename, void *buffer, uint32_t size) {
    char sname[11];
    to_short_name(filename, sname);
    fat_dir_entry_t *e = find_in_root(fs, sname);
    if (!e) return -1;
    uint32_t fsize = read_le32((const uint8_t*)&e->file_size);
    if (size > fsize) size = fsize;
    uint16_t clow = read_le16((const uint8_t*)&e->first_cluster_low);
    uint16_t chigh = 0;
    if (fs->fat_type == 32)
        chigh = read_le16((const uint8_t*)&e->first_cluster_high);
    uint32_t cluster = ((uint32_t)chigh << 16) | clow;
    uint8_t *dest = (uint8_t*)buffer;
    uint32_t remaining = size;
    uint32_t bytes_per_cluster = fs->cluster_size * 512;
    while (remaining > 0 && cluster >= 2) {
        const uint8_t *sector = cluster_to_sector(fs, cluster);
        uint32_t to_copy = (remaining < bytes_per_cluster) ? remaining : bytes_per_cluster;
        memcpy(dest, sector, to_copy);
        dest += to_copy;
        remaining -= to_copy;
        if (remaining == 0) break;
        cluster = get_fat_entry(fs, cluster);
        if (is_eoc(fs, cluster)) break;
    }
    return size - remaining;
}

int fat_list_root(fat_fs_t *fs, char names[][12], int max_count) {
    if (fs->fat_type == 32) return 0;
    uint8_t *dir = fs->image + fs->root_dir_start * 512;
    int cnt = 0;
    for (int i=0; i<(int)fs->root_entries && cnt<max_count; i++) {
        fat_dir_entry_t *e = (fat_dir_entry_t*)(dir + i*32);
        if (e->name[0]==0x00) break;
        if (e->name[0]==0xE5 || e->attr==0x0F) continue;
        char name[13];
        int pos = 0;
        for (int j=0; j<8 && e->name[j]!=' '; j++) name[pos++] = (char)e->name[j];
        if (e->name[8]!=' ') {
            name[pos++] = '.';
            for (int j=8; j<11 && e->name[j]!=' '; j++) name[pos++] = (char)e->name[j];
        }
        name[pos] = '\0';
        for (int k=0; k<12; k++) {
            names[cnt][k] = (k<=pos) ? name[k] : '\0';
        }
        cnt++;
    }
    return cnt;
}

/* ---------- Функции директорий (opendir/readdir/closedir) ---------- */
int fat_opendir(fat_fs_t *fs, uint32_t first_cluster, fat_dir_t *dir) {
    dir->fs = fs;
    if (first_cluster == 0 && fs->fat_type == 32) {
        first_cluster = fs->root_cluster;
    }
    dir->first_cluster = first_cluster;
    dir->current_cluster = first_cluster;
    dir->entry_index_in_cluster = 0;
    if (first_cluster == 0 && fs->fat_type != 32) {
        dir->is_root = 1;
        dir->total_entries = fs->root_entries;
        dir->entries_left_in_dir = fs->root_entries;
        dir->entries_per_cluster = 0;
    } else {
        dir->is_root = 0;
        uint32_t epc = (fs->cluster_size * 512) / 32;
        dir->entries_per_cluster = epc;
        dir->total_entries = 0;
        dir->entries_left_in_dir = epc;
    }
    return 0;
}

int fat_readdir(fat_dir_t *dir, fat_dir_entry_t *entry) {
    if (!dir || !entry) return -1;
    while (1) {
        if (dir->is_root) {
            if (dir->entries_left_in_dir == 0) return 0;
            uint32_t index = dir->total_entries - dir->entries_left_in_dir;
            fat_dir_entry_t *e = (fat_dir_entry_t*)(dir->fs->image +
                dir->fs->root_dir_start * 512 + index * 32);
            dir->entries_left_in_dir--;
            if (e->name[0] == 0x00) return 0;
            if (e->name[0] == 0xE5 || e->attr == 0x0F) continue;
            memcpy(entry, e, sizeof(fat_dir_entry_t));
            return 1;
        } else {
            if (dir->entries_left_in_dir == 0) {
                uint32_t next = get_fat_entry(dir->fs, dir->current_cluster);
                if (is_eoc(dir->fs, next)) return 0;
                dir->current_cluster = next;
                dir->entry_index_in_cluster = 0;
                dir->entries_left_in_dir = dir->entries_per_cluster;
            }
            const uint8_t *data = cluster_to_sector(dir->fs, dir->current_cluster);
            if (!data) return 0;
            uint32_t offset = dir->entry_index_in_cluster * 32;
            fat_dir_entry_t *e = (fat_dir_entry_t*)(data + offset);
            dir->entry_index_in_cluster++;
            dir->entries_left_in_dir--;
            if (e->name[0] == 0x00) return 0;
            if (e->name[0] == 0xE5 || e->attr == 0x0F) continue;
            memcpy(entry, e, sizeof(fat_dir_entry_t));
            return 1;
        }
    }
}

int fat_closedir(fat_dir_t *dir) { (void)dir; return 0; }

/* ---------- Выделение и освобождение кластеров ---------- */
static uint32_t find_free_cluster(fat_fs_t *fs) {
    for (uint32_t i = 2; i < fs->max_cluster; i++) {
        if (get_fat_entry(fs, i) == 0) return i;
    }
    return 0;
}

static void free_cluster_chain(fat_fs_t *fs, uint32_t start) {
    while (start >= 2) {
        uint32_t next = get_fat_entry(fs, start);
        set_fat_entry(fs, start, 0);
        if (is_eoc(fs, next)) break;
        start = next;
    }
}

/* ---------- Поиск свободной директорной записи ---------- */
static fat_dir_entry_t* find_free_dir_entry(fat_fs_t *fs, uint32_t parent_cluster,
                                            fat_dir_entry_t **next_entry_ptr)
{
    *next_entry_ptr = NULL;

    if (parent_cluster == 0 && fs->fat_type == 32)
        parent_cluster = fs->root_cluster;

    if (parent_cluster == 0) {
        uint8_t *root = fs->image + fs->root_dir_start * 512;
        for (uint32_t i = 0; i < fs->root_entries; i++) {
            fat_dir_entry_t *e = (fat_dir_entry_t*)(root + i*32);
            if (e->name[0] == 0x00 || e->name[0] == 0xE5) {
                if (e->name[0] == 0x00 && i + 1 < fs->root_entries)
                    *next_entry_ptr = (fat_dir_entry_t*)(root + (i+1)*32);
                else if (e->name[0] == 0xE5 && i + 1 < fs->root_entries)
                    *next_entry_ptr = (fat_dir_entry_t*)(root + (i+1)*32);
                return e;
            }
        }
        return NULL;
    } else {
        uint32_t cluster = parent_cluster;
        uint32_t epc = (fs->cluster_size * 512) / 32;
        while (cluster >= 2) {
            uint8_t *data = (uint8_t*)cluster_to_sector(fs, cluster);
            if (!data) break;
            for (uint32_t i = 0; i < epc; i++) {
                fat_dir_entry_t *e = (fat_dir_entry_t*)(data + i*32);
                if (e->name[0] == 0x00 || e->name[0] == 0xE5) {
                    if (i + 1 < epc)
                        *next_entry_ptr = (fat_dir_entry_t*)(data + (i+1)*32);
                    else {
                        uint32_t next = get_fat_entry(fs, cluster);
                        if (next >= 2 && !is_eoc(fs, next))
                            *next_entry_ptr = (fat_dir_entry_t*)cluster_to_sector(fs, next);
                        else if (e->name[0] == 0x00)
                            continue;
                    }
                    return e;
                }
            }
            uint32_t next = get_fat_entry(fs, cluster);
            if (is_eoc(fs, next)) break;
            cluster = next;
        }
        return NULL;
    }
}

/* ---------- Создание/удаление объектов ---------- */
int fat_mkdir(fat_fs_t *fs,
              uint32_t parent_cluster,
              const char *name)
{
    if (!fs || !name || !*name)
        return -1;

    if (name[0] == '.')
        return -1;

    char sname[11];
    to_short_name(name, sname);

    /*
     * Проверяем существование.
     */
    fat_dir_entry_t *existing =
        find_entry_in_dir(
            fs,
            parent_cluster,
            sname,
            (parent_cluster == 0 &&
             fs->fat_type != 32)
                ? 1
                : 0
        );

    if (existing)
        return -2;

    /*
     * Свободная dir entry.
     */
    fat_dir_entry_t *next_entry = NULL;

    fat_dir_entry_t *free_entry =
        find_free_dir_entry(
            fs,
            parent_cluster,
            &next_entry
        );

    if (!free_entry)
        return -3;

    /*
     * Выделяем кластер каталога.
     */
    uint32_t new_cluster =
        find_free_cluster(fs);

    if (!new_cluster)
        return -4;

    /*
     * Помечаем как EOC.
     */
    if (fs->fat_type == 12)
        set_fat_entry(fs, new_cluster, 0xFFF);
    else if (fs->fat_type == 16)
        set_fat_entry(fs, new_cluster, 0xFFFF);
    else
        set_fat_entry(fs, new_cluster, 0x0FFFFFFF);

    /*
     * Очищаем каталог.
     */
    uint8_t *data =
        (uint8_t*)cluster_to_sector(
            fs,
            new_cluster
        );

    if (!data) {
        set_fat_entry(fs, new_cluster, 0);
        return -5;
    }

    memset(data,
           0,
           fs->cluster_size * 512);

    /*
     * "."
     */
    fat_dir_entry_t *dot =
        (fat_dir_entry_t*)data;

    memset(dot->name, ' ', 11);

    dot->name[0] = '.';
    dot->attr = 0x10;

    write_le16(
        (uint8_t*)&dot->first_cluster_low,
        new_cluster & 0xFFFF
    );

    if (fs->fat_type == 32) {
        write_le16(
            (uint8_t*)&dot->first_cluster_high,
            (new_cluster >> 16) & 0xFFFF
        );
    }

    /*
     * ".."
     */
    fat_dir_entry_t *dotdot =
        (fat_dir_entry_t*)(data + 32);

    memset(dotdot->name, ' ', 11);

    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attr = 0x10;

    write_le16(
        (uint8_t*)&dotdot->first_cluster_low,
        parent_cluster & 0xFFFF
    );

    if (fs->fat_type == 32) {
        write_le16(
            (uint8_t*)&dotdot->first_cluster_high,
            (parent_cluster >> 16) & 0xFFFF
        );
    }

    /*
     * Реальная directory entry.
     */
    memset(free_entry,
           0,
           sizeof(fat_dir_entry_t));

    memcpy(free_entry->name,
           sname,
           11);

    free_entry->attr = 0x10;

    write_le16(
        (uint8_t*)&free_entry->first_cluster_low,
        new_cluster & 0xFFFF
    );

    if (fs->fat_type == 32) {
        write_le16(
            (uint8_t*)&free_entry->first_cluster_high,
            (new_cluster >> 16) & 0xFFFF
        );
    }

    write_le32(
        (uint8_t*)&free_entry->file_size,
        0
    );

    /*
     * Dirty.
     */
    uint32_t parent_lba;

    if (parent_cluster == 0 &&
        fs->fat_type != 32)
    {
        parent_lba =
            fs->root_dir_start +
            (
                (
                    (uint8_t*)free_entry -
                    (
                        fs->image +
                        fs->root_dir_start * 512
                    )
                ) / 512
            );
    }
    else
    {
        parent_lba =
            fs->data_start +
            (parent_cluster - 2) *
                fs->cluster_size +
            (
                (
                    (uint8_t*)free_entry -
                    cluster_to_sector(
                        fs,
                        parent_cluster
                    )
                ) / 512
            );
    }

    fat_mark_sector_dirty(fs, parent_lba);

    /*
     * Если использовалась последняя запись старого
     * конца каталога — сохраняем новый конец.
     */
    if (next_entry &&
        next_entry->name[0] != 0x00)
    {
        next_entry->name[0] = 0x00;
    }

    return 0;
}

int fat_rm(fat_fs_t *fs,
           uint32_t parent_cluster,
           const char *name)
{
    if (!fs || !name || !*name)
        return -1;

    if (strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0)
    {
        return -3;
    }

    char sname[11];
    to_short_name(name, sname);

    fat_dir_entry_t *entry =
        find_entry_in_dir(
            fs,
            parent_cluster,
            sname,
            (parent_cluster == 0 &&
             fs->fat_type != 32)
                ? 1
                : 0
        );

    if (!entry)
        return -2;

    uint16_t low =
        read_le16(
            (const uint8_t*)&entry->first_cluster_low
        );

    uint16_t high = 0;

    if (fs->fat_type == 32) {
        high =
            read_le16(
                (const uint8_t*)&entry->first_cluster_high
            );
    }

    uint32_t cluster =
        ((uint32_t)high << 16) | low;

    /*
     * Каталог?
     */
    if (entry->attr & 0x10) {

        /*
         * Проверяем ВСЮ цепочку каталога.
         */
        uint32_t dir_cluster = cluster;

        while (dir_cluster >= 2) {

            uint8_t *data =
                (uint8_t*)cluster_to_sector(
                    fs,
                    dir_cluster
                );

            if (!data)
                return -4;

            uint32_t epc =
                (fs->cluster_size * 512) / 32;

            for (uint32_t i = 0; i < epc; i++) {

                fat_dir_entry_t *e =
                    (fat_dir_entry_t*)
                    (data + i * 32);

                if (e->name[0] == 0x00)
                    break;

                if (e->name[0] == 0xE5 ||
                    e->attr == 0x0F)
                {
                    continue;
                }

                /*
                 * "." и ".." разрешены.
                 */
                if (e->name[0] == '.' &&
                    (e->name[1] == ' ' ||
                     e->name[1] == '.'))
                {
                    continue;
                }

                /*
                 * Нашли реальный объект.
                 */
                return -4;
            }

            uint32_t next =
                get_fat_entry(
                    fs,
                    dir_cluster
                );

            if (is_eoc(fs, next))
                break;

            if (next < 2)
                return -4;

            dir_cluster = next;
        }
    }

    /*
     * Освобождаем цепочку.
     */
    if (cluster >= 2)
        free_cluster_chain(fs, cluster);

    /*
     * Удаляем dir entry.
     */
    entry->name[0] = 0xE5;

    /*
     * Dirty parent entry.
     */
    uint32_t parent_lba;

    if (parent_cluster == 0 &&
        fs->fat_type != 32)
    {
        parent_lba =
            fs->root_dir_start +
            (
                (
                    (uint8_t*)entry -
                    (
                        fs->image +
                        fs->root_dir_start * 512
                    )
                ) / 512
            );
    }
    else
    {
        parent_lba =
            fs->data_start +
            (parent_cluster - 2) *
                fs->cluster_size +
            (
                (
                    (uint8_t*)entry -
                    cluster_to_sector(
                        fs,
                        parent_cluster
                    )
                ) / 512
            );
    }

    fat_mark_sector_dirty(fs, parent_lba);

    return 0;
}

int fat_create_file(fat_fs_t *fs, uint32_t parent_cluster, const char *name) {
    if (!name || !*name) return -1;
    char sname[11];
    to_short_name(name, sname);

    fat_dir_entry_t *existing = find_entry_in_dir(fs, parent_cluster, sname,
                                (parent_cluster == 0 && fs->fat_type != 32) ? 1 : 0);
    if (existing) return -2;

    fat_dir_entry_t *next_entry = NULL;
    fat_dir_entry_t *free_entry = find_free_dir_entry(fs, parent_cluster, &next_entry);
    if (!free_entry) return -3;

    memset(free_entry, 0, sizeof(fat_dir_entry_t));
    memcpy(free_entry->name, sname, 11);
    free_entry->attr = 0x20;
    write_le16((uint8_t*)&free_entry->first_cluster_low, 0);
    write_le32((uint8_t*)&free_entry->file_size, 0);

    uint32_t parent_lba;
    if (parent_cluster == 0 && fs->fat_type != 32)
        parent_lba = fs->root_dir_start + ((uint8_t*)free_entry - (fs->image + fs->root_dir_start*512))/512;
    else
        parent_lba = fs->data_start + (parent_cluster - 2) * fs->cluster_size
                     + ((uint8_t*)free_entry - cluster_to_sector(fs, parent_cluster))/512;
    fat_mark_sector_dirty(fs, parent_lba);

    if (next_entry && next_entry->name[0] != 0x00)
        next_entry->name[0] = 0x00;

    return 0;
}

/* ======== Финальная синхронизация ======== */
void fat_flush(fat_fs_t *fs) {
    uint8_t disk_sector[512];
    uint32_t total = fs->total_sectors;
    uint32_t written = 0;
    printf("\nFlushing FAT to disk... ");
    for (uint32_t lba = 0; lba < total; lba++) {
        if (!(fs->dirty_map[lba >> 3] & (1 << (lba & 7))))
            continue;
        uint8_t *mem_sector = fs->image + lba * 512;
        if (disk_read_sectors(lba, 1, disk_sector) == 0) {
            int changed = 0;
            for (int i = 0; i < 512; i++) {
                if (mem_sector[i] != disk_sector[i]) {
                    changed = 1;
                    break;
                }
            }
            if (changed && disk_write_sectors(lba, 1, mem_sector) == 0)
                written++;
        } else {
            if (disk_write_sectors(lba, 1, mem_sector) == 0)
                written++;
        }
    }
    memset(fs->dirty_map, 0, fs->dirty_map_size);
    printf("done (%u sectors written).\n", written);
}

/* ======== ЗАПИСЬ В ФАЙЛ ======== */

static uint32_t fat_alloc_cluster(fat_fs_t *fs, uint32_t prev_cluster) {
    uint32_t new_cluster = find_free_cluster(fs);
    if (new_cluster == 0) return 0;

    if (fs->fat_type == 12) set_fat_entry(fs, new_cluster, 0xFFF);
    else if (fs->fat_type == 16) set_fat_entry(fs, new_cluster, 0xFFFF);
    else set_fat_entry(fs, new_cluster, 0x0FFFFFFF);

    if (prev_cluster >= 2) {
        set_fat_entry(fs, prev_cluster, new_cluster);
    }

    uint8_t *cluster_data = (uint8_t*)cluster_to_sector(fs, new_cluster);
    for (uint32_t i = 0; i < fs->cluster_size * 512; i++) {
        cluster_data[i] = 0;
    }
    mark_cluster_dirty(fs, new_cluster);

    return new_cluster;
}

int fat_write_file(fat_fs_t *fs, const char *filename, const void *buffer, uint32_t size) {
    if (!fs || !filename || !buffer) return -1;

    char sname[11];
    to_short_name(filename, sname);

    fat_dir_entry_t *entry = find_in_root(fs, sname);
    if (!entry) return -1;

    uint16_t clow = read_le16((const uint8_t*)&entry->first_cluster_low);
    uint16_t chigh = 0;
    if (fs->fat_type == 32) {
        chigh = read_le16((const uint8_t*)&entry->first_cluster_high);
    }
    uint32_t old_cluster = ((uint32_t)chigh << 16) | clow;

    if (old_cluster >= 2) {
        free_cluster_chain(fs, old_cluster);
    }

    if (size == 0) {
        write_le16((uint8_t*)&entry->first_cluster_low, 0);
        if (fs->fat_type == 32) {
            write_le16((uint8_t*)&entry->first_cluster_high, 0);
        }
        write_le32((uint8_t*)&entry->file_size, 0);
        uint32_t dir_lba = fs->root_dir_start +
            (((uint8_t*)entry - (fs->image + fs->root_dir_start * 512)) / 512);
        fat_mark_sector_dirty(fs, dir_lba);
        return 0;
    }

    uint32_t bytes_per_cluster = fs->cluster_size * 512;
    uint32_t needed_clusters = (size + bytes_per_cluster - 1) / bytes_per_cluster;
    uint32_t cluster = fat_alloc_cluster(fs, 0);
    if (cluster == 0) return -1;

    uint32_t first_cluster = cluster;
    uint32_t prev = cluster;

    for (uint32_t i = 1; i < needed_clusters; i++) {
        cluster = fat_alloc_cluster(fs, prev);
        if (cluster == 0) {
            free_cluster_chain(fs, first_cluster);
            return -1;
        }
        prev = cluster;
    }

    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t remaining = size;
    cluster = first_cluster;

    while (remaining > 0 && cluster >= 2) {
        uint8_t *dst = (uint8_t*)cluster_to_sector(fs, cluster);
        uint32_t to_copy = remaining > bytes_per_cluster ? bytes_per_cluster : remaining;
        for (uint32_t i = 0; i < to_copy; i++) {
            dst[i] = src[size - remaining + i];
        }
        mark_cluster_dirty(fs, cluster);
        remaining -= to_copy;
        cluster = get_fat_entry(fs, cluster);
    }

    write_le16((uint8_t*)&entry->first_cluster_low, first_cluster & 0xFFFF);
    if (fs->fat_type == 32) {
        write_le16((uint8_t*)&entry->first_cluster_high, (first_cluster >> 16) & 0xFFFF);
    }
    write_le32((uint8_t*)&entry->file_size, size);

    uint32_t dir_lba = fs->root_dir_start +
        (((uint8_t*)entry - (fs->image + fs->root_dir_start * 512)) / 512);
    fat_mark_sector_dirty(fs, dir_lba);

    return 0;
}

int fat_append_file(fat_fs_t *fs, const char *filename, const void *buffer, uint32_t size) {
    if (!fs || !filename || !buffer || size == 0) return -1;

    char sname[11];
    to_short_name(filename, sname);
    fat_dir_entry_t *entry = find_in_root(fs, sname);
    if (!entry) return -1;

    uint32_t old_size = read_le32((const uint8_t*)&entry->file_size);
    uint32_t new_size = old_size + size;

    uint16_t clow = read_le16((const uint8_t*)&entry->first_cluster_low);
    uint16_t chigh = 0;
    if (fs->fat_type == 32) {
        chigh = read_le16((const uint8_t*)&entry->first_cluster_high);
    }
    uint32_t cluster = ((uint32_t)chigh << 16) | clow;

    if (old_size > 0 && cluster < 2) return -1;   // повреждённый файл

    uint32_t bytes_per_cluster = fs->cluster_size * 512;
    uint32_t last_cluster = cluster;
    if (last_cluster >= 2) {
        while (!is_eoc(fs, get_fat_entry(fs, last_cluster))) {
            last_cluster = get_fat_entry(fs, last_cluster);
            if (last_cluster < 2) break;
        }
    }

    uint32_t offset_in_last = old_size % bytes_per_cluster;
    uint32_t free_in_last = offset_in_last > 0 ? bytes_per_cluster - offset_in_last : 0;
    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t src_offset = 0;

    if (free_in_last > 0 && last_cluster >= 2) {
        uint8_t *dst = (uint8_t*)cluster_to_sector(fs, last_cluster);
        uint32_t to_copy = size > free_in_last ? free_in_last : size;
        for (uint32_t i = 0; i < to_copy; i++) {
            dst[offset_in_last + i] = src[i];
        }
        mark_cluster_dirty(fs, last_cluster);
        src_offset = to_copy;
    }

    while (src_offset < size) {
        uint32_t new_cluster = fat_alloc_cluster(fs, last_cluster >= 2 ? last_cluster : 0);
        if (new_cluster == 0) {
            write_le32((uint8_t*)&entry->file_size, old_size + src_offset);
            uint32_t dir_lba = fs->root_dir_start +
                (((uint8_t*)entry - (fs->image + fs->root_dir_start * 512)) / 512);
            fat_mark_sector_dirty(fs, dir_lba);
            return -1;
        }

        if (last_cluster < 2) {
            write_le16((uint8_t*)&entry->first_cluster_low, new_cluster & 0xFFFF);
            if (fs->fat_type == 32) {
                write_le16((uint8_t*)&entry->first_cluster_high, (new_cluster >> 16) & 0xFFFF);
            }
        }
        last_cluster = new_cluster;

        uint8_t *dst = (uint8_t*)cluster_to_sector(fs, new_cluster);
        uint32_t to_copy = (size - src_offset) > bytes_per_cluster ? bytes_per_cluster : (size - src_offset);
        for (uint32_t i = 0; i < to_copy; i++) {
            dst[i] = src[src_offset + i];
        }
        mark_cluster_dirty(fs, new_cluster);
        src_offset += to_copy;
    }

    write_le32((uint8_t*)&entry->file_size, new_size);
    uint32_t dir_lba = fs->root_dir_start +
        (((uint8_t*)entry - (fs->image + fs->root_dir_start * 512)) / 512);
    fat_mark_sector_dirty(fs, dir_lba);

    return 0;
}