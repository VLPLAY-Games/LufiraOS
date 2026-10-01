// VFS-интеграция USB/FAT-монтирования — см. разбор архитектуры и границ
// в fat_mount.h. Параллельно с kernel/shell/commands/mount.c (мёртвый
// кернел-native код, не трогается) — свой собственный, отдельный реестр
// монтирований, т.к. тот модуль не экспортирует свой g_mounts/статические
// хелперы наружу, а сплетать новую VFS-функциональность с мёртвым шелл-
// кодом было бы лишним риском.
#include "fat_mount.h"
#include "fat.h"
#include "../vfs/vfs.h"
#include "drivers/usb/xhci.h"
#include "system/mm/heap.h"
#include "drivers/console/console.h"
#include "system/process/process.h"
#include "lib/stddef.h"
#include "lib/string.h"

#define MAX_VFS_FAT_MOUNTS 2
#define MOUNT_PREFIX_MAX 32
#define MOUNT_MAX_IMAGE_BYTES (8u * 1024u * 1024u)
#define MOUNT_IO_RETRIES 3

typedef struct {
    int in_use;
    char prefix[MOUNT_PREFIX_MAX];
    int usb_index;
    fat_fs_t fs;
    uint8_t *image_buf;
} vfs_fat_mount_t;

static vfs_fat_mount_t g_vfs_mounts[MAX_VFS_FAT_MOUNTS];

// Открытый FAT-файл целиком читается в этот буфер при open() и пишется
// целиком обратно через fat_write_file() при каждом write() — fat_read_file()/
// fat_write_file() в fat.c сами умеют только "весь файл с начала", без
// произвольного offset (fat_lookup_path()/курсор по кластерам с offset
// никогда не были реализованы, см. fat_mount.h). Для файлов, с которыми
// реально работают на флешке (конфиги, мелкие документы), это безопаснее,
// чем самому с нуля писать offset-based чтение/запись по цепочке кластеров.
typedef struct {
    vfs_fat_mount_t *m;
    char short_name[13];
    uint8_t *buf;
    uint32_t size;
    uint32_t cap;
} fat_file_private_t;

static int root_dir_cluster(fat_fs_t *fs) {
    return (fs->fat_type == 32) ? (int)fs->root_cluster : 0;
}

// Батчит соседние "грязные" секторы в одну SCSI-команду — тот же приём,
// что mount_fat_sync_to_usb() в mount.c (кернел-native, не экспортирован
// оттуда, так что здесь — своя копия на N=2-мью отдельных монтирований).
static void sync_mount_to_usb(vfs_fat_mount_t *m) {
    uint32_t total = m->fs.total_sectors;
    uint32_t lba = 0;
    uint32_t written = 0, failed = 0;

    while (lba < total) {
        if (!(m->fs.dirty_map[lba >> 3] & (1 << (lba & 7)))) { lba++; continue; }

        uint32_t run_start = lba;
        uint32_t run_len = 0;
        while (lba < total && run_len < XHCI_MSD_MAX_BATCH_BLOCKS &&
               (m->fs.dirty_map[lba >> 3] & (1 << (lba & 7)))) {
            run_len++;
            lba++;
        }

        int ok = -1;
        for (int attempt = 0; attempt < MOUNT_IO_RETRIES; attempt++) {
            if (xhci_msd_write_blocks(m->usb_index, run_start, run_len,
                                       m->fs.image + (uint64_t)run_start * 512, 512) == 0) {
                ok = 0;
                break;
            }
        }
        if (ok == 0) written += run_len; else failed += run_len;
    }

    memset(m->fs.dirty_map, 0, m->fs.dirty_map_size);
    if (failed > 0) {
        printf("[FAT-VFS] WARNING: %u sector(s) failed to sync to usb%d ('%s')\n",
               failed, m->usb_index, m->prefix);
    }
    (void)written;
}

static vfs_fat_mount_t *find_mount_for_path(const char *path, char *rel_name_out, int rel_cap) {
    for (int i = 0; i < MAX_VFS_FAT_MOUNTS; i++) {
        if (!g_vfs_mounts[i].in_use) continue;
        vfs_fat_mount_t *m = &g_vfs_mounts[i];
        int plen = (int)strlen(m->prefix);
        if (strncmp(path, m->prefix, plen) != 0) continue;
        if (path[plen] != '\0' && path[plen] != '/') continue;

        const char *rel = path + plen;
        while (*rel == '/') rel++;

        // ГРАНИЦА (см. fat_mount.h): только корневой уровень — если после
        // префикса остаётся ещё один '/', значит это подкаталог, который
        // мы сознательно не поддерживаем. Нет strchr() в этом freestanding
        // lib/string.h (см. тот же комментарий в других местах кернела).
        int has_slash = 0;
        for (const char *s = rel; *s; s++) if (*s == '/') { has_slash = 1; break; }
        if (has_slash) continue;

        if (rel_name_out) {
            int n = 0;
            while (rel[n] && n < rel_cap - 1) { rel_name_out[n] = rel[n]; n++; }
            rel_name_out[n] = '\0';
        }
        return m;
    }
    return NULL;
}

int vfs_fat_mount_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_VFS_FAT_MOUNTS; i++) if (g_vfs_mounts[i].in_use) n++;
    return n;
}

int vfs_fat_mount(int usb_index, const char *prefix) {
    if (!prefix || prefix[0] != '/') return -1;

    for (int i = 0; i < MAX_VFS_FAT_MOUNTS; i++) {
        if (g_vfs_mounts[i].in_use && strcmp(g_vfs_mounts[i].prefix, prefix) == 0) return -2; // уже занято
    }

    int slot = -1;
    for (int i = 0; i < MAX_VFS_FAT_MOUNTS; i++) {
        if (!g_vfs_mounts[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -3; // нет свободных слотов

    uint32_t max_lba, block_size;
    if (xhci_msd_get_info(usb_index, &max_lba, &block_size) != 0) return -4; // нет такого устройства
    if (block_size != 512) return -5; // неподдерживаемый размер блока

    uint64_t total_bytes = ((uint64_t)max_lba + 1) * block_size;
    if (total_bytes > MOUNT_MAX_IMAGE_BYTES) return -6; // устройство слишком большое

    uint8_t *buf = (uint8_t *)kmalloc((size_t)total_bytes);
    if (!buf) return -7; // не хватило памяти

    uint32_t total_blocks = max_lba + 1;
    for (uint32_t lba = 0; lba < total_blocks; ) {
        uint32_t remaining = total_blocks - lba;
        uint32_t batch = remaining > XHCI_MSD_MAX_BATCH_BLOCKS ? XHCI_MSD_MAX_BATCH_BLOCKS : remaining;
        int ok = -1;
        for (int attempt = 0; attempt < MOUNT_IO_RETRIES; attempt++) {
            if (xhci_msd_read_blocks(usb_index, lba, batch, buf + (uint64_t)lba * block_size, block_size) == 0) {
                ok = 0;
                break;
            }
        }
        if (ok != 0) { kfree(buf); return -8; } // ошибка чтения
        lba += batch;
    }

    vfs_fat_mount_t *m = &g_vfs_mounts[slot];
    if (fat_init(&m->fs, buf, (uint32_t)total_bytes) != 0) { kfree(buf); return -9; } // не FAT

    m->image_buf = buf;
    m->usb_index = usb_index;
    int n = 0;
    while (prefix[n] && n < MOUNT_PREFIX_MAX - 1) { m->prefix[n] = prefix[n]; n++; }
    m->prefix[n] = '\0';
    m->in_use = 1;
    return slot;
}

int vfs_fat_unmount(const char *prefix) {
    for (int i = 0; i < MAX_VFS_FAT_MOUNTS; i++) {
        if (g_vfs_mounts[i].in_use && strcmp(g_vfs_mounts[i].prefix, prefix) == 0) {
            vfs_fat_mount_t *m = &g_vfs_mounts[i];
            sync_mount_to_usb(m);
            kfree(m->fs.dirty_map);
            kfree(m->image_buf);
            m->image_buf = NULL;
            m->in_use = 0;
            return 0;
        }
    }
    return -1;
}

/* ========== file_ops ========== */

static int fat_vfs_file_read(file_t *f, void *buf, size_t count) {
    if (!f || !f->inode || !f->inode->private_data) return -1;
    fat_file_private_t *priv = (fat_file_private_t *)f->inode->private_data;

    if (f->offset >= priv->size) return 0;
    uint32_t avail = priv->size - f->offset;
    uint32_t n = (count < avail) ? (uint32_t)count : avail;
    memcpy(buf, priv->buf + f->offset, n);
    f->offset += n;
    return (int)n;
}

static int fat_vfs_file_write(file_t *f, const void *buf, size_t count) {
    if (!f || !f->inode || !f->inode->private_data) return -1;
    fat_file_private_t *priv = (fat_file_private_t *)f->inode->private_data;

    uint32_t offset = (f->flags & O_APPEND) ? priv->size : f->offset;
    uint32_t end = offset + (uint32_t)count;

    if (end > priv->cap) {
        uint32_t new_cap = end;
        uint8_t *nb = (uint8_t *)kmalloc(new_cap);
        if (!nb) return -1;
        memcpy(nb, priv->buf, priv->size);
        kfree(priv->buf);
        priv->buf = nb;
        priv->cap = new_cap;
    }

    memcpy(priv->buf + offset, buf, count);
    if (end > priv->size) priv->size = end;
    f->offset = end;
    f->inode->size = priv->size;

    // REAL-TIME запись: сразу переписываем файл на флешке целиком и
    // синкаем "грязные" секторы на USB — см. комментарий у fat_mount.h.
    if (fat_write_file(&priv->m->fs, priv->short_name, priv->buf, priv->size) != 0) return -1;
    sync_mount_to_usb(priv->m);

    return (int)count;
}

static int fat_vfs_file_seek(file_t *f, off_t offset, int whence) {
    if (!f || !f->inode || !f->inode->private_data) return -1;
    fat_file_private_t *priv = (fat_file_private_t *)f->inode->private_data;

    switch (whence) {
        case SEEK_SET: f->offset = (uint32_t)offset; break;
        case SEEK_CUR: f->offset += (uint32_t)offset; break;
        case SEEK_END: f->offset = priv->size + (uint32_t)offset; break;
        default: return -1;
    }
    return (int)f->offset;
}

static int fat_vfs_file_close(file_t *f) {
    if (!f || !f->inode) return -1;
    if (f->inode->private_data) {
        fat_file_private_t *priv = (fat_file_private_t *)f->inode->private_data;
        if (priv->buf) kfree(priv->buf);
        kfree(priv);
        f->inode->private_data = NULL;
    }
    return 0;
}

static file_ops_t fat_vfs_file_ops = {
    .read = fat_vfs_file_read,
    .write = fat_vfs_file_write,
    .seek = fat_vfs_file_seek,
    .close = fat_vfs_file_close,
};

/* ========== inode_ops (только для корневого каталога монтирования) ===== */

typedef struct {
    vfs_fat_mount_t *m;
    fat_dir_t dir;
} fat_root_dir_private_t;

static int fat_vfs_root_readdir(inode_t *dir, void *buf, int index) {
    (void)index;
    if (!dir || !buf || !dir->private_data) return -1;
    fat_root_dir_private_t *priv = (fat_root_dir_private_t *)dir->private_data;

    fat_dir_entry_t entry;
    if (fat_readdir(&priv->dir, &entry) != 1) return 0; // конец

    vfs_dirent_t *out = (vfs_dirent_t *)buf;
    out->ino = 0;
    out->type = (entry.attr & 0x10) ? FT_DIRECTORY : FT_REGULAR;

    int pos = 0;
    for (int j = 0; j < 8 && entry.name[j] != ' '; j++) out->name[pos++] = (char)entry.name[j];
    if (entry.name[8] != ' ') {
        out->name[pos++] = '.';
        for (int j = 8; j < 11 && entry.name[j] != ' '; j++) out->name[pos++] = (char)entry.name[j];
    }
    out->name[pos] = '\0';
    return 1;
}

static inode_ops_t fat_vfs_root_ops = {
    .lookup = NULL,
    .create = NULL,
    .remove = NULL,
    .readdir = fat_vfs_root_readdir,
};

static file_t *open_root_dir(vfs_fat_mount_t *m) {
    int fd = alloc_fd();
    if (fd < 0) return NULL;

    fat_root_dir_private_t *priv = (fat_root_dir_private_t *)kmalloc(sizeof(*priv));
    if (!priv) return NULL;
    priv->m = m;
    if (fat_opendir(&m->fs, (uint32_t)root_dir_cluster(&m->fs), &priv->dir) != 0) {
        kfree(priv);
        return NULL;
    }

    inode_t *inode = vfs_create_inode(0, FT_DIRECTORY, &fat_vfs_root_ops, priv);
    if (!inode) { kfree(priv); return NULL; }

    file_t *f = alloc_file();
    if (!f) { kfree(priv); kfree(inode); return NULL; }

    f->fd = fd;
    f->inode = inode;
    f->offset = 0;
    f->flags = O_RDONLY;
    f->ops = NULL; // читают через vfs_readdir()/f->inode->ops, не f->ops

    current_fd_table->files[fd] = f;
    current_fd_table->count++;
    return f;
}

/* ========== path-level API (см. fat_mount.h) ========== */

int vfs_fat_open(const char *path, int flags) {
    char name[13];
    vfs_fat_mount_t *m = find_mount_for_path(path, name, sizeof(name));
    if (!m) return -2;

    if (name[0] == '\0') {
        // Открытие самого каталога монтирования ("/mnt/usb0").
        file_t *f = open_root_dir(m);
        return f ? f->fd : -1;
    }

    uint32_t size = 0;
    int exists = (fat_open(&m->fs, name, &size) == 0);

    if (!exists) {
        if (!(flags & O_CREAT)) return -1;
        if (fat_create_file(&m->fs, (uint32_t)root_dir_cluster(&m->fs), name) != 0) return -1;
        sync_mount_to_usb(m);
        size = 0;
    }

    int fd = alloc_fd();
    if (fd < 0) return -1;

    fat_file_private_t *priv = (fat_file_private_t *)kmalloc(sizeof(*priv));
    if (!priv) return -1;
    memset(priv, 0, sizeof(*priv));
    priv->m = m;
    int n = 0;
    while (name[n] && n < (int)sizeof(priv->short_name) - 1) { priv->short_name[n] = name[n]; n++; }
    priv->short_name[n] = '\0';

    if (!(flags & O_TRUNC) && size > 0) {
        priv->buf = (uint8_t *)kmalloc(size);
        if (!priv->buf) { kfree(priv); return -1; }
        if (fat_read_file(&m->fs, name, priv->buf, size) < 0) { kfree(priv->buf); kfree(priv); return -1; }
        priv->size = size;
        priv->cap = size;
    }

    inode_t *inode = vfs_create_inode(0, FT_REGULAR, NULL, priv);
    if (!inode) { if (priv->buf) kfree(priv->buf); kfree(priv); return -1; }
    inode->size = priv->size;

    file_t *f = alloc_file();
    if (!f) { if (priv->buf) kfree(priv->buf); kfree(priv); kfree(inode); return -1; }

    f->fd = fd;
    f->inode = inode;
    f->flags = flags;
    f->offset = (flags & O_APPEND) ? priv->size : 0;
    f->ops = &fat_vfs_file_ops;

    if (flags & O_TRUNC) { priv->size = 0; inode->size = 0; f->offset = 0; }

    current_fd_table->files[fd] = f;
    current_fd_table->count++;
    return fd;
}

int vfs_fat_mkdir(const char *path) {
    char name[13];
    vfs_fat_mount_t *m = find_mount_for_path(path, name, sizeof(name));
    if (!m) return -2;
    if (name[0] == '\0') return -1; // сам корень монтирования уже существует

    int res = fat_mkdir(&m->fs, (uint32_t)root_dir_cluster(&m->fs), name);
    if (res == 0) sync_mount_to_usb(m);
    return res;
}

int vfs_fat_unlink(const char *path) {
    char name[13];
    vfs_fat_mount_t *m = find_mount_for_path(path, name, sizeof(name));
    if (!m) return -2;
    if (name[0] == '\0') return -1;

    int res = fat_rm(&m->fs, (uint32_t)root_dir_cluster(&m->fs), name);
    if (res == 0) sync_mount_to_usb(m);
    return res;
}

inode_t *vfs_fat_lookup(const char *path) {
    char name[13];
    vfs_fat_mount_t *m = find_mount_for_path(path, name, sizeof(name));
    if (!m) return NULL;

    if (name[0] == '\0') {
        fat_root_dir_private_t *priv = (fat_root_dir_private_t *)kmalloc(sizeof(*priv));
        if (!priv) return NULL;
        priv->m = m;
        if (fat_opendir(&m->fs, (uint32_t)root_dir_cluster(&m->fs), &priv->dir) != 0) { kfree(priv); return NULL; }
        inode_t *inode = vfs_create_inode(0, FT_DIRECTORY, &fat_vfs_root_ops, priv);
        if (!inode) kfree(priv);
        return inode;
    }

    fat_dir_entry_t entry;
    if (fat_find_entry(&m->fs, (uint32_t)root_dir_cluster(&m->fs), name, &entry) != 0) return NULL;

    inode_t *inode = vfs_create_inode(0, (entry.attr & 0x10) ? FT_DIRECTORY : FT_REGULAR, NULL, NULL);
    if (inode) inode->size = read_le32((const uint8_t *)&entry.file_size);
    return inode;
}
