#include "exfat_mount.h"
#include "exfat.h"
#include "../vfs/vfs.h"
#include "drivers/usb/xhci.h"
#include "system/mm/heap.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"
#include "lib/string.h"

#define MOUNT_MAX_IMAGE_BYTES (8u * 1024u * 1024u)
#define MOUNT_IO_RETRIES 3

typedef struct {
    int in_use;
    char prefix[EXFAT_MOUNT_PREFIX_MAX];
    int usb_index;
    exfat_fs_t fs;
    uint8_t *image_buf;
} vfs_exfat_mount_t;

static vfs_exfat_mount_t g_mounts[EXFAT_MOUNT_MAX];

typedef struct {
    vfs_exfat_mount_t *m;
    exfat_handle_t h;
} exfat_file_private_t;

static void sync_mount_to_usb(vfs_exfat_mount_t *m) {
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
        printf("[EXFAT-VFS] WARNING: %u sector(s) failed to sync to usb%d ('%s')\n", failed, m->usb_index, m->prefix);
    (void)written;
}

static vfs_exfat_mount_t *find_mount_for_path(const char *path, char *rel_out, int rel_cap) {
    for (int i = 0; i < EXFAT_MOUNT_MAX; i++) {
        if (!g_mounts[i].in_use) continue;
        vfs_exfat_mount_t *m = &g_mounts[i];
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

int vfs_exfat_mount_count(void) {
    int n = 0;
    for (int i = 0; i < EXFAT_MOUNT_MAX; i++) if (g_mounts[i].in_use) n++;
    return n;
}

int vfs_exfat_mount(int usb_index, const char *prefix) {
    if (!prefix || prefix[0] != '/') return -1;
    for (int i = 0; i < EXFAT_MOUNT_MAX; i++)
        if (g_mounts[i].in_use && strcmp(g_mounts[i].prefix, prefix) == 0) return -2;

    int slot = -1;
    for (int i = 0; i < EXFAT_MOUNT_MAX; i++) if (!g_mounts[i].in_use) { slot = i; break; }
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

    vfs_exfat_mount_t *m = &g_mounts[slot];
    if (exfat_init(&m->fs, buf, (uint32_t)total_bytes) != 0) { kfree(buf); return -9; }

    m->image_buf = buf;
    m->usb_index = usb_index;
    int n = 0;
    while (prefix[n] && n < EXFAT_MOUNT_PREFIX_MAX - 1) { m->prefix[n] = prefix[n]; n++; }
    m->prefix[n] = '\0';
    m->in_use = 1;
    return slot;
}

int vfs_exfat_unmount(const char *prefix) {
    for (int i = 0; i < EXFAT_MOUNT_MAX; i++) {
        if (g_mounts[i].in_use && strcmp(g_mounts[i].prefix, prefix) == 0) {
            vfs_exfat_mount_t *m = &g_mounts[i];
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

static int exfat_vfs_file_read(file_t *f, void *buf, size_t count) {
    if (!f || !f->inode || !f->inode->private_data) return -1;
    exfat_file_private_t *priv = (exfat_file_private_t *)f->inode->private_data;
    int n = exfat_read_file(&priv->m->fs, priv->h, f->offset, buf, (uint32_t)count);
    if (n < 0) return -1;
    f->offset += (uint32_t)n;
    return n;
}

static int exfat_vfs_file_write(file_t *f, const void *buf, size_t count) {
    if (!f || !f->inode || !f->inode->private_data) return -1;
    exfat_file_private_t *priv = (exfat_file_private_t *)f->inode->private_data;

    uint64_t offset = (f->flags & O_APPEND) ? exfat_size(&priv->m->fs, priv->h) : f->offset;
    int n = exfat_write_file(&priv->m->fs, priv->h, offset, buf, (uint32_t)count);
    if (n < 0) return -1;
    f->offset = (uint32_t)(offset + (uint32_t)n);
    f->inode->size = (uint32_t)exfat_size(&priv->m->fs, priv->h);

    sync_mount_to_usb(priv->m);
    return n;
}

static int exfat_vfs_file_seek(file_t *f, off_t offset, int whence) {
    if (!f || !f->inode || !f->inode->private_data) return -1;
    exfat_file_private_t *priv = (exfat_file_private_t *)f->inode->private_data;

    switch (whence) {
        case SEEK_SET: f->offset = (uint32_t)offset; break;
        case SEEK_CUR: f->offset += (uint32_t)offset; break;
        case SEEK_END: f->offset = (uint32_t)exfat_size(&priv->m->fs, priv->h) + (uint32_t)offset; break;
        default: return -1;
    }
    return (int)f->offset;
}

static int exfat_vfs_file_close(file_t *f) {
    if (!f || !f->inode) return -1;
    if (f->inode->private_data) kfree(f->inode->private_data);
    f->inode->private_data = NULL;
    return 0;
}

static file_ops_t exfat_vfs_file_ops = {
    .read = exfat_vfs_file_read,
    .write = exfat_vfs_file_write,
    .seek = exfat_vfs_file_seek,
    .close = exfat_vfs_file_close,
};

/* ========== inode_ops (каталоги) ========== */

static int exfat_vfs_dir_readdir(inode_t *dir, void *buf, int index) {
    if (!dir || !buf || !dir->private_data) return -1;
    exfat_file_private_t *priv = (exfat_file_private_t *)dir->private_data;

    char name[256];
    exfat_handle_t h; int is_dir;
    int r = exfat_readdir(&priv->m->fs, priv->h, index, name, &h, &is_dir);
    if (r != 1) return r;

    vfs_dirent_t *out = (vfs_dirent_t *)buf;
    out->ino = h;
    out->type = is_dir ? FT_DIRECTORY : FT_REGULAR;
    strcpy(out->name, name);
    return 1;
}

static inode_ops_t exfat_vfs_dir_ops = {
    .lookup = NULL, .create = NULL, .remove = NULL,
    .readdir = exfat_vfs_dir_readdir,
};

static inode_t *make_inode(vfs_exfat_mount_t *m, exfat_handle_t h) {
    exfat_file_private_t *priv = (exfat_file_private_t *)kmalloc(sizeof(*priv));
    if (!priv) return NULL;
    priv->m = m;
    priv->h = h;

    int is_dir = exfat_is_dir(&m->fs, h);
    inode_t *inode = vfs_create_inode((uint32_t)h, is_dir ? FT_DIRECTORY : FT_REGULAR,
                                       is_dir ? &exfat_vfs_dir_ops : NULL, priv);
    if (!inode) { kfree(priv); return NULL; }
    inode->size = (uint32_t)exfat_size(&m->fs, h);
    return inode;
}

/* ========== path-level API ========== */

int vfs_exfat_open(const char *path, int flags) {
    char rel[256];
    vfs_exfat_mount_t *m = find_mount_for_path(path, rel, sizeof(rel));
    if (!m) return -2;

    exfat_handle_t h;
    if (rel[0] == '\0') {
        h = EXFAT_ROOT_HANDLE;
    } else if (exfat_lookup_path(&m->fs, rel, &h) != 0) {
        if (!(flags & O_CREAT)) return -1;
        exfat_handle_t parent; char leaf[256];
        if (exfat_resolve_parent(&m->fs, rel, &parent, leaf, sizeof(leaf)) != 0) return -1;
        if (exfat_create(&m->fs, parent, leaf, 0, &h) != 0) return -1;
        sync_mount_to_usb(m);
    }

    if ((flags & O_TRUNC) && !exfat_is_dir(&m->fs, h) && exfat_size(&m->fs, h) != 0) {
        exfat_truncate(&m->fs, h);
        sync_mount_to_usb(m);
    }

    int fd = alloc_fd();
    if (fd < 0) return -1;
    file_t *f = alloc_file();
    if (!f) return -1;

    inode_t *vinode = make_inode(m, h);
    if (!vinode) return -1;

    int is_dir = exfat_is_dir(&m->fs, h);
    f->fd = fd;
    f->inode = vinode;
    f->flags = flags;
    f->ops = is_dir ? NULL : &exfat_vfs_file_ops;
    f->offset = is_dir ? 0 : ((flags & O_APPEND) ? (uint32_t)exfat_size(&m->fs, h) : 0);

    current_fd_table->files[fd] = f;
    current_fd_table->count++;
    return fd;
}

int vfs_exfat_mkdir(const char *path) {
    char rel[256];
    vfs_exfat_mount_t *m = find_mount_for_path(path, rel, sizeof(rel));
    if (!m) return -2;
    if (rel[0] == '\0') return -1;

    exfat_handle_t parent; char leaf[256];
    if (exfat_resolve_parent(&m->fs, rel, &parent, leaf, sizeof(leaf)) != 0) return -1;

    exfat_handle_t out;
    int r = exfat_create(&m->fs, parent, leaf, 1, &out);
    if (r == 0) sync_mount_to_usb(m);
    return r;
}

int vfs_exfat_unlink(const char *path) {
    char rel[256];
    vfs_exfat_mount_t *m = find_mount_for_path(path, rel, sizeof(rel));
    if (!m) return -2;
    if (rel[0] == '\0') return -1;

    exfat_handle_t parent; char leaf[256];
    if (exfat_resolve_parent(&m->fs, rel, &parent, leaf, sizeof(leaf)) != 0) return -1;

    int r = exfat_unlink(&m->fs, parent, leaf);
    if (r == 0) sync_mount_to_usb(m);
    return r;
}

inode_t *vfs_exfat_lookup(const char *path) {
    char rel[256];
    vfs_exfat_mount_t *m = find_mount_for_path(path, rel, sizeof(rel));
    if (!m) return NULL;

    exfat_handle_t h;
    if (rel[0] == '\0') h = EXFAT_ROOT_HANDLE;
    else if (exfat_lookup_path(&m->fs, rel, &h) != 0) return NULL;

    return make_inode(m, h);
}
