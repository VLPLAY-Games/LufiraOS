#include "fat.h"

// Простая memcpy, если нет в lib
static void* memcpy(void* dest, const void* src, unsigned int n) {
    char* d = (char*)dest;
    const char* s = (const char*)src;
    while (n--) *d++ = *s++;
    return dest;
}

static uint32_t read_le32(const uint8_t *p) {
    return p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24);
}
static uint16_t read_le16(const uint8_t *p) {
    return p[0] | (p[1]<<8);
}

static const uint8_t* cluster_to_sector(fat_fs_t *fs, uint32_t cluster) {
    if (cluster < 2) return 0;
    uint32_t lba = fs->data_start + (cluster - 2) * fs->cluster_size;
    return fs->image + lba * 512;
}

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
    uint32_t total_sec = fs->bpb.total_sectors_16 ? fs->bpb.total_sectors_16 : fs->bpb.total_sectors_32;

    if (total_sec * bytes_per_sec > image_size) return -2;

    uint32_t sectors_per_fat = fs->bpb.sectors_per_fat_16;
    if (sectors_per_fat == 0) {
        sectors_per_fat = read_le32(raw+36); // FAT32
    }

    uint32_t fat_start = reserved;
    uint32_t root_dir_start = fat_start + fats * sectors_per_fat;
    uint32_t data_start = root_dir_start + (fs->bpb.root_entries * 32 + bytes_per_sec - 1) / bytes_per_sec;

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
    return 0;
}

static uint32_t get_fat_entry(fat_fs_t *fs, uint32_t cluster) {
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

static void to_short_name(const char *filename, char out[11]) {
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

static fat_dir_entry_t* find_in_root(fat_fs_t *fs, const char* name8_3) {
    if (fs->fat_type == 32) return 0; // для простоты опущено
    uint8_t *dir = fs->image + fs->root_dir_start * 512;
    for (int i=0; i<(int)fs->root_entries; i++) {
        fat_dir_entry_t *e = (fat_dir_entry_t*)(dir + i*32);
        if (e->name[0]==0x00) break;
        if (e->name[0]==0xE5 || e->attr==0x0F) continue;
        int match = 1;
        for (int j=0; j<11; j++) {
            if (e->name[j] != name8_3[j]) { match=0; break; }
        }
        if (match) return e;
    }
    return 0;
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
        if (cluster >= 0xFFF8) break;
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
        for (int j=0; j<8 && e->name[j]!=' '; j++) name[pos++] = e->name[j];
        if (e->name[8]!=' ') {
            name[pos++] = '.';
            for (int j=8; j<11 && e->name[j]!=' '; j++) name[pos++] = e->name[j];
        }
        name[pos] = '\0';
        for (int k=0; k<12; k++) {
            names[cnt][k] = (k<=pos) ? name[k] : '\0';
        }
        cnt++;
    }
    return cnt;
}