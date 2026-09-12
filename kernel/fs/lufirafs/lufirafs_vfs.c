// VFS-обвязка над LufiraFS — прямой аналог kernel/fs/fat/fat_vfs.c, тот же
// принцип (см. подробный разбор в комментариях там): каждый inode_t,
// который видит VFS, несёт в private_data маленькую структуру с номером
// LufiraFS-inode и (для директорий) курсором обхода.
//
// Пути здесь ВСЕГДА разрешаются от КОРНЯ — как и было у FAT
// (fat_lookup_path безусловно стартует с dir_cluster=0). Это то, что видят
// системные вызовы (userland ELF шлёт свои пути syscall'ами). Команды
// шелла (cd/ls/...), наоборот, работают через lufirafs_* напрямую и сами
// передают cwd-inode — см. kernel/shell/commands/filesystem.c.
#include "lufirafs.h"
#include "system/mm/heap.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"
#include "lib/string.h"

extern lufirafs_t lufirafs;

typedef struct {
    uint32_t ino;
    int is_dir;
    lufirafs_dir_t dir; // используется только для директорий
} lufirafs_private_t;

static inode_t *lufirafs_inode_lookup(inode_t *dir, const char *name);
static int lufirafs_inode_create(inode_t *dir, const char *name, file_type_t type);
static int lufirafs_inode_remove(inode_t *dir, const char *name);
static int lufirafs_inode_readdir(inode_t *dir, void *buf, int index);

static inode_ops_t lufirafs_inode_ops = {
    .lookup = lufirafs_inode_lookup,
    .create = lufirafs_inode_create,
    .remove = lufirafs_inode_remove,
    .readdir = lufirafs_inode_readdir,
};

int vfs_lufirafs_create(const char *path);
int vfs_lufirafs_mkdir(const char *path);
int vfs_lufirafs_unlink(const char *path);
inode_t *vfs_lufirafs_lookup(const char *path);
inode_t *vfs_lufirafs_get_root(void);

/* ========== ОПЕРАЦИИ ДЛЯ ФАЙЛОВ ========== */

static int lufirafs_file_read(file_t *f, void *buf, size_t count) {
    if (!f || !f->inode || !f->inode->private_data) return -1;
    lufirafs_private_t *priv = (lufirafs_private_t*)f->inode->private_data;
    if (priv->is_dir) return -1;

    int n = lufirafs_read(&lufirafs, priv->ino, f->offset, buf, (uint32_t)count);
    if (n > 0) f->offset += (uint32_t)n;
    return n;
}

static int lufirafs_file_write(file_t *f, const void *buf, size_t count) {
    if (!f || !f->inode || !f->inode->private_data) return -1;
    lufirafs_private_t *priv = (lufirafs_private_t*)f->inode->private_data;
    if (priv->is_dir) return -1;

    uint32_t offset = (f->flags & O_APPEND) ? f->inode->size : f->offset;
    int n = lufirafs_write(&lufirafs, priv->ino, offset, buf, (uint32_t)count);
    if (n > 0) {
        f->offset = offset + (uint32_t)n;
        lufirafs_inode_t inode;
        if (lufirafs_read_inode(&lufirafs, priv->ino, &inode) == 0) f->inode->size = inode.size;
        lufirafs_sync(&lufirafs);
    }
    return n;
}

static int lufirafs_file_seek(file_t *f, off_t offset, int whence) {
    if (!f || !f->inode) return -1;
    uint32_t size = f->inode->size;

    switch (whence) {
        case SEEK_SET: f->offset = (uint32_t)offset; break;
        case SEEK_CUR: f->offset += (uint32_t)offset; break;
        case SEEK_END: f->offset = size + (uint32_t)offset; break;
        default: return -1;
    }
    if (f->offset > size) f->offset = size;
    return (int)f->offset;
}

static int lufirafs_file_close(file_t *f) {
    if (!f || !f->inode) return -1;
    if (f->inode->private_data) {
        kfree(f->inode->private_data);
        f->inode->private_data = NULL;
    }
    return 0;
}

static file_ops_t lufirafs_file_ops = {
    .read = lufirafs_file_read,
    .write = lufirafs_file_write,
    .seek = lufirafs_file_seek,
    .close = lufirafs_file_close,
};

/* ========== ОПЕРАЦИИ ДЛЯ ДИРЕКТОРИЙ ========== */

static int lufirafs_dir_read(file_t *f, void *buf, size_t count) {
    (void)count;
    if (!f || !f->inode || !buf) return -1;
    return vfs_readdir(f->fd, buf);
}

static int lufirafs_dir_close(file_t *f) {
    if (!f || !f->inode) return -1;
    if (f->inode->private_data) {
        kfree(f->inode->private_data);
        f->inode->private_data = NULL;
    }
    return 0;
}

static file_ops_t lufirafs_dir_ops = {
    .read = lufirafs_dir_read,
    .write = NULL,
    .seek = NULL,
    .close = lufirafs_dir_close,
};

/* ========== ВСПОМОГАТЕЛЬНОЕ: строим inode_t по номеру LufiraFS-inode ===== */

static inode_t *build_inode(uint32_t ino) {
    lufirafs_inode_t disk_inode;
    if (lufirafs_read_inode(&lufirafs, ino, &disk_inode) != 0) return NULL;

    lufirafs_private_t *priv = (lufirafs_private_t*)kmalloc(sizeof(lufirafs_private_t));
    if (!priv) return NULL;
    memset(priv, 0, sizeof(*priv));
    priv->ino = ino;
    priv->is_dir = (disk_inode.mode == LUFIRAFS_MODE_DIR);
    if (priv->is_dir) lufirafs_opendir(&lufirafs, ino, &priv->dir);

    inode_t *inode = vfs_create_inode(ino, priv->is_dir ? FT_DIRECTORY : FT_REGULAR,
                                       &lufirafs_inode_ops, priv);
    if (!inode) { kfree(priv); return NULL; }

    inode->size = disk_inode.size;
    return inode;
}

/* ========== VFS OPEN ========== */

int vfs_open_lufirafs(const char *path, int flags) {
    if (!path || !*path) return -1;
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v') return -1;

    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, lufirafs.sb.root_inode, path, &ino) != 0) return -1;

    int fd = alloc_fd();
    if (fd < 0) return -1;

    inode_t *inode = build_inode(ino);
    if (!inode) return -1;

    file_t *f = alloc_file();
    if (!f) { kfree(inode->private_data); kfree(inode); return -1; }

    f->fd = fd;
    f->inode = inode;
    f->flags = flags;
    f->offset = (flags & O_APPEND) ? inode->size : 0;
    f->ops = (inode->type == FT_DIRECTORY) ? &lufirafs_dir_ops : &lufirafs_file_ops;

    if ((flags & O_TRUNC) && inode->type != FT_DIRECTORY) {
        lufirafs_private_t *priv = (lufirafs_private_t*)inode->private_data;
        lufirafs_truncate(&lufirafs, priv->ino, 0);
        inode->size = 0;
        f->offset = 0;
    }

    current_fd_table->files[fd] = f;
    current_fd_table->count++;

    printf("[VFS] Opened '%s' as fd=%d\n", path, fd);
    return fd;
}

/* ========== inode_ops ========== */

static int lufirafs_inode_readdir(inode_t *dir, void *buf, int index) {
    (void)index;
    if (!dir || !buf || !dir->private_data) return -1;
    lufirafs_private_t *priv = (lufirafs_private_t*)dir->private_data;
    if (!priv->is_dir) return -1;

    lufirafs_dirent_t ent;
    if (lufirafs_readdir(&priv->dir, &ent) != 0) return 0; // конец

    vfs_dirent_t *out = (vfs_dirent_t*)buf;
    out->ino = ent.inode;

    lufirafs_inode_t disk_inode;
    lufirafs_read_inode(&lufirafs, ent.inode, &disk_inode);
    out->type = (disk_inode.mode == LUFIRAFS_MODE_DIR) ? FT_DIRECTORY : FT_REGULAR;

    strcpy(out->name, ent.name); // ent.name всегда короче sizeof(out->name) (256)
    return 1;
}

static inode_t *lufirafs_inode_lookup(inode_t *dir, const char *name) {
    if (!dir || !name || !*name || !dir->private_data) return NULL;
    lufirafs_private_t *parent = (lufirafs_private_t*)dir->private_data;
    if (!parent->is_dir) return NULL;

    // Отдельный, разовый курсор обхода — не трогает parent->dir (тот
    // принадлежит readdir() этого же inode и должен остаться на месте).
    uint32_t ino = 0;
    lufirafs_dir_t tmp;
    lufirafs_opendir(&lufirafs, parent->ino, &tmp);
    lufirafs_dirent_t ent;
    int found = 0;
    while (lufirafs_readdir(&tmp, &ent) == 0) {
        if (strcmp(ent.name, name) == 0) { ino = ent.inode; found = 1; break; }
    }
    if (!found) return NULL;

    return build_inode(ino);
}

static int lufirafs_inode_create(inode_t *dir, const char *name, file_type_t type) {
    if (!dir || !name || !*name || !dir->private_data) return -1;
    lufirafs_private_t *priv = (lufirafs_private_t*)dir->private_data;
    if (!priv->is_dir) return -1;

    uint32_t mode = (type == FT_DIRECTORY) ? LUFIRAFS_MODE_DIR : LUFIRAFS_MODE_FILE;
    uint32_t out_ino;
    int res = lufirafs_create(&lufirafs, priv->ino, name, mode, &out_ino);
    if (res == 0) lufirafs_sync(&lufirafs);
    return res;
}

static int lufirafs_inode_remove(inode_t *dir, const char *name) {
    if (!dir || !name || !*name || !dir->private_data) return -1;
    lufirafs_private_t *priv = (lufirafs_private_t*)dir->private_data;
    if (!priv->is_dir) return -1;

    int res = lufirafs_unlink(&lufirafs, priv->ino, name);
    if (res == 0) lufirafs_sync(&lufirafs);
    return res;
}

/* ========== path-level VFS хелперы ========== */

int vfs_lufirafs_create(const char *path) {
    if (!path || !*path) return -1;

    uint32_t parent;
    char name[LUFIRAFS_MAX_NAME + 1];
    if (lufirafs_resolve_parent(&lufirafs, lufirafs.sb.root_inode, path, &parent, name) != 0) return -1;

    uint32_t out_ino;
    int res = lufirafs_create(&lufirafs, parent, name, LUFIRAFS_MODE_FILE, &out_ino);
    if (res == 0) lufirafs_sync(&lufirafs);
    return (res == 0) ? 0 : -2;
}

int vfs_lufirafs_mkdir(const char *path) {
    if (!path || !*path) return -1;
    if (strcmp(path, "/") == 0) return -2;

    uint32_t parent;
    char name[LUFIRAFS_MAX_NAME + 1];
    if (lufirafs_resolve_parent(&lufirafs, lufirafs.sb.root_inode, path, &parent, name) != 0) return -1;

    uint32_t out_ino;
    int res = lufirafs_create(&lufirafs, parent, name, LUFIRAFS_MODE_DIR, &out_ino);
    if (res == 0) lufirafs_sync(&lufirafs);
    return res;
}

int vfs_lufirafs_unlink(const char *path) {
    if (!path || !*path) return -1;
    if (strcmp(path, "/") == 0) return -2;

    uint32_t parent;
    char name[LUFIRAFS_MAX_NAME + 1];
    if (lufirafs_resolve_parent(&lufirafs, lufirafs.sb.root_inode, path, &parent, name) != 0) return -1;

    int res = lufirafs_unlink(&lufirafs, parent, name);
    if (res == 0) lufirafs_sync(&lufirafs);
    return res;
}

inode_t *vfs_lufirafs_lookup(const char *path) {
    if (!path || !*path) return NULL;

    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, lufirafs.sb.root_inode, path, &ino) != 0) return NULL;
    return build_inode(ino);
}

inode_t *vfs_lufirafs_get_root(void) {
    return build_inode(lufirafs.sb.root_inode);
}
