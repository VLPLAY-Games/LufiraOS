#include "fat.h"
#include "../vfs/vfs.h"
#include "system/mm/heap.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"

// Внешняя глобальная FAT
extern fat_fs_t fatfs;

// Приватные данные для FAT inode
typedef struct {
    uint32_t cluster;        // Первый кластер
    fat_dir_entry_t entry;   // Информация о файле
} fat_private_t;

// ========== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ==========

static void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static int strlen(const char *s) {
    int len = 0;
    while (*s++) len++;
    return len;
}

// ========== РЕАЛИЗАЦИЯ ОПЕРАЦИЙ ДЛЯ FAT-ФАЙЛОВ ==========

static int fat_file_read(file_t *f, void *buf, size_t count) {
    if (!f || !f->inode || !f->inode->private_data) return -1;
    
    fat_private_t *priv = (fat_private_t *)f->inode->private_data;
    
    // Читаем из файла, начиная с offset
    uint8_t *dest = (uint8_t *)buf;
    uint32_t remaining = (uint32_t)count;
    uint32_t file_size = read_le32((const uint8_t*)&priv->entry.file_size);
    
    // Проверяем границы
    if (f->offset >= file_size) return 0;
    if (f->offset + remaining > file_size) {
        remaining = file_size - f->offset;
    }
    
    // Читаем цепочку кластеров
    uint32_t cluster = priv->cluster;
    uint32_t bytes_per_cluster = fatfs.cluster_size * 512;
    
    // Пропускаем кластеры до offset
    uint32_t skip_clusters = f->offset / bytes_per_cluster;
    uint32_t offset_in_cluster = f->offset % bytes_per_cluster;
    
    for (uint32_t i = 0; i < skip_clusters && cluster >= 2; i++) {
        cluster = read_le16((const uint8_t*)&((fat_dir_entry_t*)0)->first_cluster_low);
        // TODO: правильно проходить по цепочке кластеров через FAT
    }
    
    // Читаем данные
    uint32_t total_read = 0;
    while (remaining > 0 && cluster >= 2 && cluster < 0xFFFFFF8) {
        // Вычисляем адрес кластера
        const uint8_t *cluster_data = (const uint8_t*)(fatfs.image + 
            (fatfs.data_start + (cluster - 2) * fatfs.cluster_size) * 512);
        
        uint32_t to_copy = bytes_per_cluster - offset_in_cluster;
        if (to_copy > remaining) to_copy = remaining;
        
        // Копируем данные
        for (uint32_t i = 0; i < to_copy; i++) {
            dest[total_read + i] = cluster_data[offset_in_cluster + i];
        }
        
        total_read += to_copy;
        remaining -= to_copy;
        offset_in_cluster = 0;
        
        // Получаем следующий кластер из FAT
        // (упрощённо, для FAT16)
        if (fatfs.fat_type == 16) {
            uint8_t *fat = fatfs.image + fatfs.fat_start * 512;
            uint32_t offset = cluster * 2;
            cluster = fat[offset] | (fat[offset + 1] << 8);
        } else if (fatfs.fat_type == 32) {
            uint8_t *fat = fatfs.image + fatfs.fat_start * 512;
            uint32_t offset = cluster * 4;
            cluster = fat[offset] | (fat[offset + 1] << 8) | 
                     (fat[offset + 2] << 16) | (fat[offset + 3] << 24);
            cluster &= 0x0FFFFFFF;
        } else {
            break;
        }
    }
    
    f->offset += total_read;
    return (int)total_read;
}

static int fat_file_write(file_t *f, const void *buf, size_t count) {
    (void)f;
    (void)buf;
    (void)count;
    // Запись пока не поддерживается
    return -1;
}

static int fat_file_seek(file_t *f, off_t offset, int whence) {
    if (!f || !f->inode) return -1;
    
    uint32_t file_size = 0;
    if (f->inode->private_data) {
        fat_private_t *priv = (fat_private_t *)f->inode->private_data;
        file_size = read_le32((const uint8_t*)&priv->entry.file_size);
    }
    
    switch (whence) {
        case SEEK_SET:
            f->offset = (uint32_t)offset;
            break;
        case SEEK_CUR:
            f->offset += (uint32_t)offset;
            break;
        case SEEK_END:
            f->offset = file_size + (uint32_t)offset;
            break;
        default:
            return -1;
    }
    
    // Ограничиваем размером файла
    if (f->offset > file_size) f->offset = file_size;
    
    return (int)f->offset;
}

static int fat_file_close(file_t *f) {
    if (!f || !f->inode) return -1;
    
    // Освобождаем приватные данные
    if (f->inode->private_data) {
        kfree(f->inode->private_data);
        f->inode->private_data = NULL;
    }
    
    return 0;
}

// Операции для FAT-файлов
static file_ops_t fat_file_ops = {
    .read = fat_file_read,
    .write = fat_file_write,
    .seek = fat_file_seek,
    .close = fat_file_close,
};

// ========== ОПЕРАЦИИ С FAT-ДИРЕКТОРИЯМИ ==========

static int fat_dir_read(file_t *f, void *buf, size_t count) {
    (void)f;
    (void)buf;
    (void)count;
    return 0;  // TODO
}

static int fat_dir_close(file_t *f) {
    (void)f;
    return 0;
}

static file_ops_t fat_dir_ops = {
    .read = fat_dir_read,
    .write = NULL,
    .seek = NULL,
    .close = fat_dir_close,
};

// ========== ИНТЕГРАЦИЯ FAT В VFS ==========

// Поиск файла в FAT
static int fat_lookup_path(const char *path, fat_dir_entry_t *out_entry, uint32_t *out_cluster) {
    if (!path || !*path) return -1;
    
    // Пропускаем начальный слеш
    if (path[0] == '/') path++;
    if (*path == '\0') return -1;
    
    // Копируем имя (убираем завершающий слеш если есть)
    char name[256];
    int i = 0;
    while (path[i] && path[i] != '/' && i < 255) {
        name[i] = path[i];
        i++;
    }
    name[i] = '\0';
    
    // Ищем в корне (пока только корень)
    // В будущем: ходить по директориям
    char sname[11];
    // Простое преобразование в 8.3
    for (int j = 0; j < 11; j++) sname[j] = ' ';
    
    int j = 0;
    for (int k = 0; name[k] && j < 8; k++) {
        if (name[k] == '.') break;
        char c = name[k];
        if (c >= 'a' && c <= 'z') c -= 32;
        sname[j++] = c;
    }
    
    // Ищем расширение
    const char *ext = name;
    while (*ext && *ext != '.') ext++;
    if (*ext == '.') {
        ext++;
        j = 8;
        for (int k = 0; ext[k] && j < 11; k++) {
            char c = ext[k];
            if (c >= 'a' && c <= 'z') c -= 32;
            sname[j++] = c;
        }
    }
    
    // Ищем в корневой директории
    if (fatfs.fat_type == 32) {
        // TODO: обход цепочки кластеров для FAT32
    }
    
    // Поиск в корневой директории
    uint8_t *dir = fatfs.image + fatfs.root_dir_start * 512;
    for (uint32_t i = 0; i < fatfs.root_entries; i++) {
        fat_dir_entry_t *e = (fat_dir_entry_t *)(dir + i * 32);
        
        if (e->name[0] == 0x00) break;
        if (e->name[0] == 0xE5 || e->attr == 0x0F) continue;
        
        int match = 1;
        for (int k = 0; k < 11; k++) {
            if (e->name[k] != (uint8_t)sname[k]) {
                match = 0;
                break;
            }
        }
        
        if (match) {
            // Копируем найденный entry
            for (int k = 0; k < sizeof(fat_dir_entry_t); k++) {
                ((uint8_t*)out_entry)[k] = ((uint8_t*)e)[k];
            }
            
            uint16_t clow = read_le16((const uint8_t*)&e->first_cluster_low);
            uint16_t chigh = 0;
            if (fatfs.fat_type == 32) {
                chigh = read_le16((const uint8_t*)&e->first_cluster_high);
            }
            *out_cluster = ((uint32_t)chigh << 16) | clow;
            
            return 0;
        }
    }
    
    return -1;
}

// ========== ОБНОВЛЁННЫЙ vfs_open ДЛЯ FAT ==========

int vfs_open_fat(const char *path, int flags) {
    if (!path || !*path) return -1;
    
    // Специальные устройства
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v') {
        return -1;
    }
    
    const char *fname = path;
    if (fname[0] == '/') fname++;
    
    fat_dir_entry_t entry;
    uint32_t cluster = 0;
    
    if (fat_lookup_path(fname, &entry, &cluster) != 0) {
        return -1;
    }
    
    // Используем alloc_fd из vfs.h
    int fd = alloc_fd();
    if (fd < 0) return -1;
    
    // Создаём приватные данные
    fat_private_t *priv = (fat_private_t *)kmalloc(sizeof(fat_private_t));
    if (!priv) return -1;
    
    memset(priv, 0, sizeof(fat_private_t));
    priv->cluster = cluster;
    // Копируем entry
    for (int i = 0; i < sizeof(fat_dir_entry_t); i++) {
        ((uint8_t*)&priv->entry)[i] = ((uint8_t*)&entry)[i];
    }
    
    // Создаём inode
    inode_t *inode = vfs_create_inode(0, 
        (entry.attr & 0x10) ? FT_DIRECTORY : FT_REGULAR,
        NULL, priv);
    if (!inode) {
        kfree(priv);
        return -1;
    }
    inode->size = read_le32((const uint8_t*)&entry.file_size);
    
    // Создаём file структуру
    file_t *f = (file_t *)kmalloc(sizeof(file_t));
    if (!f) {
        kfree(priv);
        kfree(inode);
        return -1;
    }
    
    memset(f, 0, sizeof(file_t));
    f->fd = fd;
    f->inode = inode;
    f->flags = flags;
    f->offset = 0;
    f->ops = ((entry.attr & 0x10) ? &fat_dir_ops : &fat_file_ops);
    
    // Добавляем в глобальную таблицу
    for (int i = 0; i < MAX_FILES_SYSTEM; i++) {
        if (file_table[i] == NULL) {
            file_table[i] = f;
            break;
        }
    }
    
    current_fd_table->files[fd] = f;
    
    printf("[VFS] Opened '%s' as fd=%d\n", fname, fd);
    
    return fd;
}
