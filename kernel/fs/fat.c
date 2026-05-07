#include "fat.h"

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

static void set_fat_entry(fat_fs_t *fs, uint32_t cluster, uint32_t value) {
    if (cluster < 2) return;
    uint8_t *fat = fs->image + fs->fat_start * 512;
    uint32_t offset;
    switch (fs->fat_type) {
        case 12:
            offset = cluster + (cluster >> 1);
            if (cluster & 1) {
                fat[offset] = (fat[offset] & 0x0F) | ((value & 0xF) << 4);
                fat[offset+1] = (value >> 4) & 0xFF;
            } else {
                fat[offset] = value & 0xFF;
                fat[offset+1] = (fat[offset+1] & 0xF0) | ((value >> 8) & 0x0F);
            }
            break;
        case 16:
            offset = cluster * 2;
            fat[offset] = value & 0xFF;
            fat[offset+1] = (value >> 8) & 0xFF;
            break;
        case 32:
            offset = cluster * 4;
            fat[offset] = value & 0xFF;
            fat[offset+1] = (value >> 8) & 0xFF;
            fat[offset+2] = (value >> 16) & 0xFF;
            fat[offset+3] = (value >> 24) & 0xFF;
            break;
    }
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
    if (fs->fat_type == 32) return 0;
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

// ---------- НОВЫЕ ФУНКЦИИ ----------

int fat_opendir(fat_fs_t *fs, uint32_t first_cluster, fat_dir_t *dir) {
    dir->fs = fs;
    dir->first_cluster = first_cluster;
    dir->current_cluster = first_cluster;
    dir->sector_buf = 0;
    dir->sector_offset = 0;
    dir->entries_left = 0;
    dir->total_entries = 0;
    dir->is_root = (first_cluster == 0);

    if (first_cluster == 0) {
        // корень
        dir->total_entries = fs->root_entries;
        dir->entries_left = fs->root_entries;
        dir->sector_offset = 0;
        dir->sector_buf = 0; // будет выделено при чтении
        return 0;
    }
    // иначе обычная директория
    uint32_t bytes_per_cluster = fs->cluster_size * 512;
    dir->total_entries = bytes_per_cluster / 32; // приблизительно
    dir->entries_left = 0; // заполним при первом readdir
    return 0;
}

static uint8_t* read_dir_sector(fat_dir_t *dir, uint32_t *remaining) {
    fat_fs_t *fs = dir->fs;
    if (dir->is_root) {
        uint8_t *root = fs->image + fs->root_dir_start * 512;
        uint32_t off = dir->sector_offset;
        *remaining = dir->entries_left;
        return root + off * 32;
    } else {
        if (dir->current_cluster < 2) return 0;
        uint32_t byte_offset = dir->sector_offset * 32;
        uint32_t bytes_per_cluster = fs->cluster_size * 512;
        if (byte_offset >= bytes_per_cluster) {
            // переходим к следующему кластеру
            uint32_t next = get_fat_entry(fs, dir->current_cluster);
            if (next >= 0xFFF8) return 0;
            dir->current_cluster = next;
            dir->sector_offset = 0;
            byte_offset = 0;
        }
        const uint8_t *data = cluster_to_sector(fs, dir->current_cluster);
        if (!data) return 0;
        *remaining = (bytes_per_cluster - byte_offset) / 32;
        // динамически выделяем буфер? Нет, будем работать напрямую с образом
        // но нужно вернуть указатель на запись в образе.
        return (uint8_t*)(data + byte_offset);
    }
}

int fat_readdir(fat_dir_t *dir, fat_dir_entry_t *entry) {
    if (!dir || !entry) return -1;
    while (1) {
        if (dir->entries_left == 0) {
            if (dir->is_root) return 0; // больше нет
            // обычная директория: проверим следующий кластер
            if (dir->current_cluster < 2 || get_fat_entry(dir->fs, dir->current_cluster) >= 0xFFF8)
                return 0;
            dir->current_cluster = get_fat_entry(dir->fs, dir->current_cluster);
            dir->sector_offset = 0;
            uint32_t bytes_per_cluster = dir->fs->cluster_size * 512;
            dir->entries_left = bytes_per_cluster / 32;
            dir->total_entries += dir->entries_left;
        }
        fat_dir_entry_t *e = (fat_dir_entry_t*)((uint8_t*)dir->fs->image);
        if (dir->is_root) {
            e = (fat_dir_entry_t*)(dir->fs->image + dir->fs->root_dir_start * 512 +
                                   (dir->total_entries - dir->entries_left) * 32);
        } else {
            uint32_t byte_off = (dir->total_entries - dir->entries_left) * 32;
            const uint8_t *data = cluster_to_sector(dir->fs, dir->current_cluster);
            e = (fat_dir_entry_t*)(data + byte_off);
        }
        dir->entries_left--;
        if (e->name[0] == 0x00) return 0; // конец
        if (e->name[0] == 0xE5 || e->attr == 0x0F) continue; // пропускаем
        memcpy(entry, e, sizeof(fat_dir_entry_t));
        return 1;
    }
}

int fat_closedir(fat_dir_t *dir) {
    // ничего не делаем
    return 0;
}

// Поиск свободного кластера (возвращает номер или 0)
static uint32_t find_free_cluster(fat_fs_t *fs) {
    uint32_t max_cluster = (fs->total_sectors - fs->data_start) / fs->cluster_size + 2;
    for (uint32_t i = 2; i < max_cluster; i++) {
        uint32_t val = get_fat_entry(fs, i);
        if (val == 0) return i;
    }
    return 0;
}

// Освободить цепочку кластеров
static void free_cluster_chain(fat_fs_t *fs, uint32_t start) {
    while (start >= 2 && start < 0xFFF8) {
        uint32_t next = get_fat_entry(fs, start);
        set_fat_entry(fs, start, 0);
        if (next >= 0xFFF8) break;
        start = next;
    }
}

// Найти свободную запись в директории, возвращает указатель на entry в образе
static fat_dir_entry_t* find_free_dir_entry(fat_fs_t *fs, uint32_t parent_cluster) {
    if (parent_cluster == 0) {
        uint8_t *root = fs->image + fs->root_dir_start * 512;
        for (uint32_t i = 0; i < fs->root_entries; i++) {
            fat_dir_entry_t *e = (fat_dir_entry_t*)(root + i*32);
            if (e->name[0] == 0x00 || e->name[0] == 0xE5) return e;
        }
        return 0;
    } else {
        uint32_t cluster = parent_cluster;
        while (cluster >= 2) {
            uint8_t *data = (uint8_t*)cluster_to_sector(fs, cluster);
            uint32_t entries_per_cluster = (fs->cluster_size * 512) / 32;
            for (uint32_t i = 0; i < entries_per_cluster; i++) {
                fat_dir_entry_t *e = (fat_dir_entry_t*)(data + i*32);
                if (e->name[0] == 0x00 || e->name[0] == 0xE5) return e;
            }
            uint32_t next = get_fat_entry(fs, cluster);
            if (next >= 0xFFF8) break;
            cluster = next;
        }
        // Если не нашли, можно попытаться расширить цепочку (но пока не реализовано)
        return 0;
    }
}

int fat_mkdir(fat_fs_t *fs, uint32_t parent_cluster, const char *name) {
    if (!name || !*name) return -1;
    char sname[11];
    to_short_name(name, sname);
    // проверка на существование
    fat_dir_entry_t *existing = 0;
    if (parent_cluster == 0) {
        existing = find_in_root(fs, sname);
    } else {
        // упрощённо: ищем в parent_cluster (не реализовано полное, но можно сказать, что mkdir только в корне)
        // Для совместимости с текущим кодом будем искать в корне.
        existing = find_in_root(fs, sname);
    }
    if (existing) return -2; // уже существует

    fat_dir_entry_t *free_entry = find_free_dir_entry(fs, parent_cluster);
    if (!free_entry) return -3; // нет места

    // Выделяем один кластер для новой директории
    uint32_t new_cluster = find_free_cluster(fs);
    if (!new_cluster) return -4;

    // Помечаем кластер как последний
    set_fat_entry(fs, new_cluster, 0xFFFF);

    // Инициализируем . и .. записи внутри нового кластера
    uint8_t *data = (uint8_t*)cluster_to_sector(fs, new_cluster);
    memset(data, 0, 512 * fs->cluster_size);
    fat_dir_entry_t *dot = (fat_dir_entry_t*)data;
    memset(dot->name, ' ', 11);
    dot->name[0] = '.';
    dot->attr = 0x10; // Directory
    write_le16((uint8_t*)&dot->first_cluster_low, new_cluster & 0xFFFF);
    // .. запись указывает на parent_cluster (0 для корня)
    fat_dir_entry_t *dotdot = (fat_dir_entry_t*)(data + 32);
    memset(dotdot->name, ' ', 11);
    dotdot->name[0] = '.'; dotdot->name[1] = '.';
    dotdot->attr = 0x10;
    write_le16((uint8_t*)&dotdot->first_cluster_low, parent_cluster & 0xFFFF);

    // Заполняем свободную запись в родительской директории
    memset(free_entry, 0, sizeof(fat_dir_entry_t));
    memcpy(free_entry->name, sname, 11);
    free_entry->attr = 0x10;
    write_le16((uint8_t*)&free_entry->first_cluster_low, new_cluster & 0xFFFF);
    write_le32((uint8_t*)&free_entry->file_size, 0);

    return 0;
}

int fat_rm(fat_fs_t *fs, uint32_t parent_cluster, const char *name) {
    if (!name || !*name) return -1;
    char sname[11];
    to_short_name(name, sname);
    fat_dir_entry_t *entry = 0;
    if (parent_cluster == 0) {
        entry = find_in_root(fs, sname);
    } else {
        // ищем в родительской директории (пока только для корня)
        entry = find_in_root(fs, sname);
    }
    if (!entry) return -2;
    if (entry->name[0] == '.' && entry->name[1] == ' ') return -3; // запрещено удалять .
    if (entry->attr & 0x10) { // это директория
        // проверим, что она пуста (кроме . и ..)
        uint32_t dir_cluster = read_le16((const uint8_t*)&entry->first_cluster_low);
        uint8_t *data = (uint8_t*)cluster_to_sector(fs, dir_cluster);
        uint32_t entries_per_cluster = (fs->cluster_size * 512) / 32;
        for (uint32_t i = 2; i < entries_per_cluster; i++) {
            fat_dir_entry_t *e = (fat_dir_entry_t*)(data + i*32);
            if (e->name[0] != 0x00 && e->name[0] != 0xE5) return -4; // не пуста
        }
        free_cluster_chain(fs, dir_cluster);
    } else {
        // обычный файл
        uint32_t cluster = read_le16((const uint8_t*)&entry->first_cluster_low);
        free_cluster_chain(fs, cluster);
    }
    // помечаем запись как удалённую
    entry->name[0] = 0xE5;
    return 0;
}

int fat_create_file(fat_fs_t *fs, uint32_t parent_cluster, const char *name) {
    (void)fs; (void)parent_cluster; (void)name;
    return -1; // не реализовано
}