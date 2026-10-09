#include "ext2_mount.h"
#include "ext2.h"
#include "../vfs/vfs.h"
#include "drivers/usb/xhci.h"
#include "system/mm/heap.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"
#include "lib/string.h"

#define MOUNT_MAX_IMAGE_BYTES (8u * 1024u * 1024u) // тот же потолок, что у FAT — см. fat_mount.h
#define MOUNT_IO_RETRIES 3

typedef struct {
    int in_use;
    char prefix[EXT2_MOUNT_PREFIX_MAX];
    int usb_index;
    ext2_fs_t fs;
    uint8_t *image_buf;
} vfs_ext2_mount_t;

static vfs_ext2_mount_t g_mounts[EXT2_MOUNT_MAX];

typedef struct {
    vfs_ext2_mount_t *m;
    uint32_t ino;
} ext2_file_private_t;

// Батчит соседние "грязные" секторы в одну SCSI-команду — дословно тот же
// приём, что sync_mount_to_usb() в fat_mount.c (своя копия: тот модуль
// ничего не экспортирует наружу).
static void sync_mount_to_usb(vfs_ext2_mount_t *m) {
    uint32_t total = m->fs.image_size / 512;
    uint32_t lba = 0, written = 0, failed = 0;

    while (lba < total) {
        if (!(m->fs.dirty_map[lba >> 3] & (1 << (lba & 7)))) { lba++; continue; }

        uint32_t run_start = lba, run_len = 0;
        while (lba < total && run_len < XHCI_MSD_MAX_BATCH_BLOCKS &&
               (m->fs.dirty_map[lba >> 3] & (1 << (lba & 7)))) {
            run_len++; lba++;
        }

        int ok = -1;
        for (int attempt = 0; attempt < MOUNT_IO_RETRIES; attempt++) {
            if (xhci_msd_write_blocks(m->usb_index, run_start, run_len,
                                       m->fs.image + (uint64_t)run_start * 512, 512) == 0) {
                ok = 0; break;
            }
        }
        if (ok == 0) written += run_len; else failed += run_len;
    }

    memset(m->fs.dirty_map, 0, m->fs.dirty_map_size);
    if (failed > 0)
        printf("[EXT2-VFS] WARNING: %u sector(s) failed to sync to usb%d ('%s')\n", failed, m->usb_index, m->prefix);
    (void)written;
}

static vfs_ext2_mount_t *find_mount_for_path(const char *path, char *rel_out, int rel_cap) {
    for (int i = 0; i < EXT2_MOUNT_MAX; i++) {
        if (!g_mounts[i].in_use) continue;
        vfs_ext2_mount_t *m = &g_mounts[i];
        int plen = (int)strlen(m->prefix);
        if (strncmp(path, m->prefix, plen) != 0) continue;
        if (path[plen] != '\0' && path[plen] != '/') continue;

        const char *rel = path + plen;
        while (*rel == '/') rel++;
        if (rel_out) {
            int n = 0;
            while (rel[n] && n < rel_cap - 1) { rel_out[n] = rel[n]; n++; }
            rel_out[n] = '\0';
        }
        return m;
    }
    return NULL;
}

int vfs_ext2_mount_count(void) {
    int n = 0;
    for (int i = 0; i < EXT2_MOUNT_MAX; i++) if (g_mounts[i].in_use) n++;
    return n;
}

int vfs_ext2_mount(int usb_index, const char *prefix) {
    if (!prefix || prefix[0] != '/') return -1;
    for (int i = 0; i < EXT2_MOUNT_MAX; i++)
        if (g_mounts[i].in_use && strcmp(g_mounts[i].prefix, prefix) == 0) return -2;

    int slot = -1;
    for (int i = 0; i < EXT2_MOUNT_MAX; i++) if (!g_mounts[i].in_use) { slot = i; break; }
    if (slot < 0) return -3;

    uint32_t max_lba, block_size;
    if (xhci_msd_get_info(usb_index, &max_lba, &block_size) != 0) return -4;
    if (block_size != 512) return -5;

    uint64_t total_bytes = ((uint64_t)max_lba + 1) * block_size;
    if (total_bytes > MOUNT_MAX_IMAGE_BYTES) return -6;

    uint8_t *buf = (uint8_t *)kmalloc((size_t)total_bytes);
    if (!buf) return -7;

    uint32_t total_blocks = max_lba + 1;
    for (uint32_t lba = 0; lba < total_blocks; ) {
        uint32_t remaining = total_blocks - lba;
        uint32_t batch = remaining > XHCI_MSD_MAX_BATCH_BLOCKS ? XHCI_MSD_MAX_BATCH_BLOCKS : remaining;
        int ok = -1;
        for (int attempt = 0; attempt < MOUNT_IO_RETRIES; attempt++) {
            if (xhci_msd_read_blocks(usb_index, lba, batch, buf + (uint64_t)lba * block_size, block_size) == 0) {
                ok = 0; break;
            }
        }
        if (ok != 0) { kfree(buf); return -8; }
        lba += batch;
    }

    vfs_ext2_mount_t *m = &g_mounts[slot];
    if (ext2_init(&m->fs, buf, (uint32_t)total_bytes) != 0) { kfree(buf); return -9; }

    m->image_buf = buf;
    m->usb_index = usb_index;
    int n = 0;
    while (prefix[n] && n < EXT2_MOUNT_PREFIX_MAX - 1) { m->prefix[n] = prefix[n]; n++; }
    m->prefix[n] = '\0';
    m->in_use = 1;
    return slot;
}

int vfs_ext2_unmount(const char *prefix) {
    for (int i = 0; i < EXT2_MOUNT_MAX; i++) {
        if (g_mounts[i].in_use && strcmp(g_mounts[i].prefix, prefix) == 0) {
            vfs_ext2_mount_t *m = &g_mounts[i];
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

static int ext2_vfs_file_read(file_t *f, void *buf, size_t count) {
    if (!f || !f->inode || !f->inode->private_data) return -1;
    ext2_file_private_t *priv = (ext2_file_private_t *)f->inode->private_data;
    int n = ext2_read_file(&priv->m->fs, priv->ino, f->offset, buf, (uint32_t)count);
    if (n < 0) return -1;
    f->offset += (uint32_t)n;
    return n;
}

static int ext2_vfs_file_write(file_t *f, const void *buf, size_t count) {
    if (!f || !f->inode || !f->inode->private_data) return -1;
    ext2_file_private_t *priv = (ext2_file_private_t *)f->inode->private_data;

    ext2_inode_t inode;
    if (ext2_read_inode(&priv->m->fs, priv->ino, &inode) != 0) return -1;
    uint32_t offset = (f->flags & O_APPEND) ? inode.i_size : f->offset;

    int n = ext2_write_file(&priv->m->fs, priv->ino, offset, buf, (uint32_t)count);
    if (n < 0) return -1;
    f->offset = offset + (uint32_t)n;

    ext2_read_inode(&priv->m->fs, priv->ino, &inode);
    f->inode->size = inode.i_size;

    sync_mount_to_usb(priv->m); // real-time, как у FAT/см. fat_mount.h
    return n;
}

static int ext2_vfs_file_seek(file_t *f, off_t offset, int whence) {
    if (!f || !f->inode || !f->inode->private_data) return -1;
    ext2_file_private_t *priv = (ext2_file_private_t *)f->inode->private_data;

    switch (whence) {
        case SEEK_SET: f->offset = (uint32_t)offset; break;
        case SEEK_CUR: f->offset += (uint32_t)offset; break;
        case SEEK_END: {
            ext2_inode_t inode;
            if (ext2_read_inode(&priv->m->fs, priv->ino, &inode) != 0) return -1;
            f->offset = inode.i_size + (uint32_t)offset;
            break;
        }
        default: return -1;
    }
    return (int)f->offset;
}

static int ext2_vfs_file_close(file_t *f) {
    if (!f || !f->inode) return -1;
    if (f->inode->private_data) kfree(f->inode->private_data); // просто {m,ino} — не владеет данными ФС
    f->inode->private_data = NULL;
    return 0;
}

static file_ops_t ext2_vfs_file_ops = {
    .read = ext2_vfs_file_read,
    .write = ext2_vfs_file_write,
    .seek = ext2_vfs_file_seek,
    .close = ext2_vfs_file_close,
};

/* ========== inode_ops (каталоги — любой, не только корень монтирования) */

static int ext2_vfs_dir_readdir(inode_t *dir, void *buf, int index) {
    if (!dir || !buf || !dir->private_data) return -1;
    ext2_file_private_t *priv = (ext2_file_private_t *)dir->private_data;

    char name[EXT2_NAME_LEN + 1];
    uint32_t ino; int is_dir;
    int r = ext2_readdir(&priv->m->fs, priv->ino, index, name, &ino, &is_dir);
    if (r != 1) return r;

    vfs_dirent_t *out = (vfs_dirent_t *)buf;
    out->ino = ino;
    out->type = is_dir ? FT_DIRECTORY : FT_REGULAR;
    strcpy(out->name, name);
    return 1;
}

static inode_ops_t ext2_vfs_dir_ops = {
    .lookup = NULL, .create = NULL, .remove = NULL,
    .readdir = ext2_vfs_dir_readdir,
};

static inode_t *make_inode(vfs_ext2_mount_t *m, uint32_t ino, const ext2_inode_t *info) {
    ext2_file_private_t *priv = (ext2_file_private_t *)kmalloc(sizeof(*priv));
    if (!priv) return NULL;
    priv->m = m;
    priv->ino = ino;

    int is_dir = ext2_is_dir(info);
    inode_t *inode = vfs_create_inode(ino, is_dir ? FT_DIRECTORY : FT_REGULAR,
                                       is_dir ? &ext2_vfs_dir_ops : NULL, priv);
    if (!inode) { kfree(priv); return NULL; }
    inode->size = info->i_size;
    return inode;
}

/* ========== path-level API (см. ext2_mount.h) ========== */

int vfs_ext2_open(const char *path, int flags) {
    char rel[256];
    vfs_ext2_mount_t *m = find_mount_for_path(path, rel, sizeof(rel));
    if (!m) return -2;

    uint32_t ino;
    if (rel[0] == '\0') {
        ino = EXT2_ROOT_INO;
    } else if (ext2_lookup_path(&m->fs, rel, &ino) != 0) {
        if (!(flags & O_CREAT)) return -1;
        uint32_t parent; char leaf[256];
        if (ext2_resolve_parent(&m->fs, rel, &parent, leaf, sizeof(leaf)) != 0) return -1;
        if (ext2_create(&m->fs, parent, leaf, &ino) != 0) return -1;
        sync_mount_to_usb(m);
    }

    ext2_inode_t info;
    if (ext2_read_inode(&m->fs, ino, &info) != 0) return -1;

    if ((flags & O_TRUNC) && !ext2_is_dir(&info) && info.i_size != 0) {
        ext2_truncate(&m->fs, ino);
        ext2_read_inode(&m->fs, ino, &info);
        sync_mount_to_usb(m);
    }

    int fd = alloc_fd();
    if (fd < 0) return -1;
    file_t *f = alloc_file();
    if (!f) return -1;

    inode_t *vinode = make_inode(m, ino, &info);
    if (!vinode) return -1;

    f->fd = fd;
    f->inode = vinode;
    f->flags = flags;
    f->ops = ext2_is_dir(&info) ? NULL : &ext2_vfs_file_ops;
    f->offset = (ext2_is_dir(&info)) ? 0 : ((flags & O_APPEND) ? info.i_size : 0);

    current_fd_table->files[fd] = f;
    current_fd_table->count++;
    return fd;
}

int vfs_ext2_mkdir(const char *path) {
    char rel[256];
    vfs_ext2_mount_t *m = find_mount_for_path(path, rel, sizeof(rel));
    if (!m) return -2;
    if (rel[0] == '\0') return -1;

    uint32_t parent; char leaf[256];
    if (ext2_resolve_parent(&m->fs, rel, &parent, leaf, sizeof(leaf)) != 0) return -1;

    uint32_t out_ino;
    int r = ext2_mkdir(&m->fs, parent, leaf, &out_ino);
    if (r == 0) sync_mount_to_usb(m);
    return r;
}

int vfs_ext2_unlink(const char *path) {
    char rel[256];
    vfs_ext2_mount_t *m = find_mount_for_path(path, rel, sizeof(rel));
    if (!m) return -2;
    if (rel[0] == '\0') return -1;

    uint32_t parent; char leaf[256];
    if (ext2_resolve_parent(&m->fs, rel, &parent, leaf, sizeof(leaf)) != 0) return -1;

    int r = ext2_unlink(&m->fs, parent, leaf);
    if (r == 0) sync_mount_to_usb(m);
    return r;
}

inode_t *vfs_ext2_lookup(const char *path) {
    char rel[256];
    vfs_ext2_mount_t *m = find_mount_for_path(path, rel, sizeof(rel));
    if (!m) return NULL;

    uint32_t ino;
    if (rel[0] == '\0') {
        ino = EXT2_ROOT_INO;
    } else if (ext2_lookup_path(&m->fs, rel, &ino) != 0) {
        return NULL;
    }

    ext2_inode_t info;
    if (ext2_read_inode(&m->fs, ino, &info) != 0) return NULL;
    return make_inode(m, ino, &info);
}
