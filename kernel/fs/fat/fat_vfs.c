#include "fat.h"
#include "../vfs/vfs.h"
#include "system/mm/heap.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"

extern fat_fs_t fatfs;

typedef struct {
    uint32_t cluster;
    fat_dir_entry_t entry;
} fat_private_t;

/* ========== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ========== */

static void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static void *memcpy(void *dest, const void *src, size_t n) {
    char *d = (char *)dest;
    const char *s = (const char *)src;
    while (n--) *d++ = *s++;
    return dest;
}

static int strlen(const char *s) {
    int len = 0;
    while (*s++) len++;
    return len;
}

/* ========== ОПЕРАЦИИ ДЛЯ FAT-ФАЙЛОВ ========== */

static int fat_file_read(file_t *f, void *buf, size_t count) {
    if (!f || !f->inode || !f->inode->private_data) return -1;

    fat_private_t *priv = (fat_private_t *)f->inode->private_data;
    uint32_t file_size = read_le32((const uint8_t*)&priv->entry.file_size);

    if (f->offset >= file_size) return 0;

    uint32_t remaining = (uint32_t)count;
    if (f->offset + remaining > file_size) {
        remaining = file_size - f->offset;
    }

    uint32_t cluster = priv->cluster;
    uint32_t bytes_per_cluster = fatfs.cluster_size * 512;
    uint32_t off = f->offset;

    /* Пропускаем кластеры до offset */
    while (off >= bytes_per_cluster && cluster >= 2 && !is_eoc(&fatfs, cluster)) {
        cluster = get_fat_entry(&fatfs, cluster);
        off -= bytes_per_cluster;
    }
    uint32_t offset_in_cluster = off;

    uint8_t *dest = (uint8_t *)buf;
    uint32_t total_read = 0;

    while (remaining > 0 && cluster >= 2 && !is_eoc(&fatfs, cluster)) {
        const uint8_t *cluster_data = (const uint8_t*)(fatfs.image +
            (fatfs.data_start + (cluster - 2) * fatfs.cluster_size) * 512);

        uint32_t to_copy = bytes_per_cluster - offset_in_cluster;
        if (to_copy > remaining) to_copy = remaining;

        for (uint32_t i = 0; i < to_copy; i++) {
            dest[total_read + i] = cluster_data[offset_in_cluster + i];
        }

        total_read += to_copy;
        remaining -= to_copy;
        offset_in_cluster = 0;

        cluster = get_fat_entry(&fatfs, cluster);
    }

    f->offset += total_read;
    return (int)total_read;
}

static int fat_file_write(file_t *f, const void *buf, size_t count) {
    (void)f; (void)buf; (void)count;
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
        case SEEK_SET: f->offset = (uint32_t)offset; break;
        case SEEK_CUR: f->offset += (uint32_t)offset; break;
        case SEEK_END: f->offset = file_size + (uint32_t)offset; break;
        default: return -1;
    }

    if (f->offset > file_size) f->offset = file_size;
    return (int)f->offset;
}

static int fat_file_close(file_t *f) {
    if (!f || !f->inode) return -1;
    if (f->inode->private_data) {
        kfree(f->inode->private_data);
        f->inode->private_data = NULL;
    }
    return 0;
}

static file_ops_t fat_file_ops = {
    .read = fat_file_read,
    .write = fat_file_write,
    .seek = fat_file_seek,
    .close = fat_file_close,
};

/* ========== ОПЕРАЦИИ С FAT-ДИРЕКТОРИЯМИ ========== */

static int fat_dir_read(file_t *f, void *buf, size_t count) {
    (void)f; (void)buf; (void)count;
    return 0;
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

/* ========== ПОИСК ФАЙЛА В FAT ========== */

static int fat_lookup_path(const char *path, fat_dir_entry_t *out_entry, uint32_t *out_cluster) {
    if (!path || !*path) return -1;
    if (path[0] == '/') path++;
    if (*path == '\0') return -1;

    /* Извлекаем имя первого компонента */
    char name[256];
    int i = 0;
    while (path[i] && path[i] != '/' && i < 255) {
        name[i] = path[i];
        i++;
    }
    name[i] = '\0';

    char sname[11];
    to_short_name(name, sname);

    if (fatfs.fat_type == 32) {
        uint32_t dir_cluster = fatfs.root_cluster;
        while (dir_cluster >= 2) {
            const uint8_t *data = (const uint8_t*)(fatfs.image +
                (fatfs.data_start + (dir_cluster - 2) * fatfs.cluster_size) * 512);
            uint32_t epc = (fatfs.cluster_size * 512) / 32;
            for (uint32_t i = 0; i < epc; i++) {
                fat_dir_entry_t *e = (fat_dir_entry_t *)(data + i * 32);
                if (e->name[0] == 0x00) break;
                if (e->name[0] == 0xE5 || e->attr == 0x0F) continue;

                int match = 1;
                for (int k = 0; k < 11; k++) {
                    if (e->name[k] != (uint8_t)sname[k]) { match = 0; break; }
                }
                if (match) {
                    memcpy(out_entry, e, sizeof(fat_dir_entry_t));
                    uint16_t clow = read_le16((const uint8_t*)&e->first_cluster_low);
                    uint16_t chigh = read_le16((const uint8_t*)&e->first_cluster_high);
                    *out_cluster = ((uint32_t)chigh << 16) | clow;
                    return 0;
                }
            }
            dir_cluster = get_fat_entry(&fatfs, dir_cluster);
        }
        return -1;
    } else {
        uint8_t *dir = fatfs.image + fatfs.root_dir_start * 512;
        for (uint32_t i = 0; i < fatfs.root_entries; i++) {
            fat_dir_entry_t *e = (fat_dir_entry_t *)(dir + i * 32);
            if (e->name[0] == 0x00) break;
            if (e->name[0] == 0xE5 || e->attr == 0x0F) continue;

            int match = 1;
            for (int k = 0; k < 11; k++) {
                if (e->name[k] != (uint8_t)sname[k]) { match = 0; break; }
            }
            if (match) {
                memcpy(out_entry, e, sizeof(fat_dir_entry_t));
                uint16_t clow = read_le16((const uint8_t*)&e->first_cluster_low);
                *out_cluster = clow;
                return 0;
            }
        }
        return -1;
    }
}

/* ========== VFS OPEN ДЛЯ FAT ========== */

int vfs_open_fat(const char *path, int flags) {
    if (!path || !*path) return -1;

    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v')
        return -1;

    const char *fname = path;
    if (fname[0] == '/') fname++;

    fat_dir_entry_t entry;
    uint32_t cluster = 0;
    if (fat_lookup_path(fname, &entry, &cluster) != 0) return -1;

    int fd = alloc_fd();
    if (fd < 0) return -1;

    fat_private_t *priv = (fat_private_t *)kmalloc(sizeof(fat_private_t));
    if (!priv) return -1;
    memset(priv, 0, sizeof(fat_private_t));
    priv->cluster = cluster;
    memcpy(&priv->entry, &entry, sizeof(fat_dir_entry_t));

    inode_t *inode = vfs_create_inode(0,
        (entry.attr & 0x10) ? FT_DIRECTORY : FT_REGULAR,
        NULL, priv);
    if (!inode) {
        kfree(priv);
        return -1;
    }
    inode->size = read_le32((const uint8_t*)&entry.file_size);

    file_t *f = alloc_file();
    if (!f) {
        kfree(priv);
        kfree(inode);
        return -1;
    }

    f->fd = fd;
    f->inode = inode;
    f->flags = flags;
    f->offset = 0;
    f->ops = ((entry.attr & 0x10) ? &fat_dir_ops : &fat_file_ops);

    current_fd_table->files[fd] = f;
    current_fd_table->count++;

    printf("[VFS] Opened '%s' as fd=%d\n", fname, fd);
    return fd;
}