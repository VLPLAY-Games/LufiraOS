#include "fat.h"
#include "../vfs/vfs.h"
#include "system/mm/heap.h"
#include "drivers/console/console.h"
#include "lib/stddef.h"

extern fat_fs_t fatfs;

typedef struct {
    uint32_t cluster;
    fat_dir_entry_t entry;

    /*
     * Используется только для directories.
     */
    fat_dir_t dir;
    int is_dir;
} fat_private_t;

/* ========== FORWARD DECLARATIONS ========== */

static inode_t *fat_inode_lookup(inode_t *dir,
                                 const char *name);

static int fat_inode_create(inode_t *dir,
                            const char *name,
                            file_type_t type);

static int fat_inode_remove(inode_t *dir,
                            const char *name);

static int fat_inode_readdir(inode_t *dir,
                             void *buf,
                             int index);

static inode_ops_t fat_inode_ops = {
    .lookup = fat_inode_lookup,
    .create = fat_inode_create,
    .remove = fat_inode_remove,
    .readdir = fat_inode_readdir
};

/*
 * VFS-level FAT functions.
 */
int vfs_fat_create(const char *path);
int vfs_fat_mkdir(const char *path);
int vfs_fat_unlink(const char *path);

inode_t *vfs_fat_lookup(const char *path);
inode_t *vfs_fat_get_root(void);

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

static int fat_dir_read(file_t *f,
                        void *buf,
                        size_t count)
{
    (void)count;

    if (!f ||
        !f->inode ||
        !buf)
    {
        return -1;
    }

    return vfs_readdir(f->fd, buf);
}

static int fat_dir_close(file_t *f)
{
    if (!f || !f->inode)
        return -1;

    if (f->inode->private_data) {

        kfree(f->inode->private_data);

        f->inode->private_data = NULL;
    }

    return 0;
}

static file_ops_t fat_dir_ops = {
    .read = fat_dir_read,
    .write = NULL,
    .seek = NULL,
    .close = fat_dir_close,
};

/* ========== ПОИСК ФАЙЛА В FAT ========== */

int fat_lookup_path(fat_fs_t *fs,
                    const char *path,
                    fat_dir_entry_t *out_entry,
                    uint32_t *out_cluster)
{
    if (!fs || !path || !*path || !out_entry || !out_cluster)
        return -1;

    /* Пропускаем ведущие '/' */
    while (*path == '/')
        path++;

    if (!*path)
        return -1;

    char component[256];
    uint32_t dir_cluster = 0;

    while (*path) {

        /* Пропускаем повторные '/' */
        while (*path == '/')
            path++;

        if (!*path)
            break;

        /* Извлекаем один компонент */
        int len = 0;

        while (path[len] &&
               path[len] != '/' &&
               len < 255)
        {
            component[len] = path[len];
            len++;
        }

        component[len] = '\0';

        if (len == 0)
            break;

        /*
         * Обработка "."
         */
        if (component[0] == '.' &&
            component[1] == '\0')
        {
            path += len;
            continue;
        }

        /*
         * ".."
         *
         * Полноценный parent traversal позже можно
         * сделать через '..' запись.
         * Пока не уходим выше root.
         */
        if (component[0] == '.' &&
            component[1] == '.' &&
            component[2] == '\0')
        {
            if (dir_cluster == 0) {
                path += len;
                continue;
            }

            fat_dir_entry_t dotdot;

            if (fat_find_entry(fs,
                                dir_cluster,
                                "..",
                                &dotdot) != 0)
            {
                return -1;
            }

            uint16_t low =
                read_le16(
                    (const uint8_t*)&dotdot.first_cluster_low
                );

            uint16_t high = 0;

            if (fs->fat_type == 32) {
                high =
                    read_le16(
                        (const uint8_t*)&dotdot.first_cluster_high
                    );
            }

            dir_cluster =
                ((uint32_t)high << 16) | low;

            /*
             * Для FAT32 root parent может быть 0.
             */
            path += len;
            continue;
        }

        fat_dir_entry_t entry;

        if (fat_find_entry(fs,
                            dir_cluster,
                            component,
                            &entry) != 0)
        {
            return -1;
        }

        uint16_t low =
            read_le16(
                (const uint8_t*)&entry.first_cluster_low
            );

        uint16_t high = 0;

        if (fs->fat_type == 32) {
            high =
                read_le16(
                    (const uint8_t*)&entry.first_cluster_high
                );
        }

        uint32_t entry_cluster =
            ((uint32_t)high << 16) | low;

        /*
         * Есть ли ещё компоненты?
         */
        const char *rest = path + len;

        while (*rest == '/')
            rest++;

        if (*rest) {

            /*
             * Промежуточный компонент обязан быть каталогом.
             */
            if (!(entry.attr & 0x10))
                return -1;

            dir_cluster = entry_cluster;
            path = rest;
            continue;
        }

        /*
         * Это последний компонент.
         */
        memcpy(out_entry,
               &entry,
               sizeof(fat_dir_entry_t));

        *out_cluster = entry_cluster;

        return 0;
    }

    return -1;
}

int fat_resolve_parent(fat_fs_t *fs,
                       const char *path,
                       uint32_t *out_parent_cluster,
                       char *out_name)
{
    if (!fs ||
        !path ||
        !out_parent_cluster ||
        !out_name)
    {
        return -1;
    }

    /*
     * Копируем путь во временный буфер.
     */
    char tmp[512];

    int len = 0;

    while (path[len] && len < 511) {
        tmp[len] = path[len];
        len++;
    }

    tmp[len] = '\0';

    if (len == 0)
        return -1;

    /*
     * Убираем конечные '/'.
     */
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
        len--;
    }

    /*
     * Ищем последний '/'.
     */
    int slash = -1;

    for (int i = 0; i < len; i++) {
        if (tmp[i] == '/')
            slash = i;
    }

    /*
     * Файл непосредственно в root.
     */
    if (slash <= 0) {

        const char *name =
            (slash == 0)
                ? &tmp[1]
                : tmp;

        if (!*name)
            return -1;

        int i = 0;

        while (name[i] && i < 255) {
            out_name[i] = name[i];
            i++;
        }

        out_name[i] = '\0';

        *out_parent_cluster = 0;

        return 0;
    }

    /*
     * Выделяем parent path.
     */
    char parent_path[512];

    for (int i = 0; i < slash; i++)
        parent_path[i] = tmp[i];

    parent_path[slash] = '\0';

    /*
     * Последнее имя.
     */
    const char *name =
        &tmp[slash + 1];

    if (!*name)
        return -1;

    int name_len = 0;

    while (name[name_len] &&
           name_len < 255)
    {
        out_name[name_len] = name[name_len];
        name_len++;
    }

    out_name[name_len] = '\0';

    /*
     * Root.
     */
    if (parent_path[0] == '\0' ||
        (parent_path[0] == '/' &&
         parent_path[1] == '\0'))
    {
        *out_parent_cluster = 0;
        return 0;
    }

    /*
     * Ищем родительский каталог.
     */
    fat_dir_entry_t parent_entry;
    uint32_t parent_cluster;

    if (fat_lookup_path(fs,
                        parent_path,
                        &parent_entry,
                        &parent_cluster) != 0)
    {
        return -1;
    }

    if (!(parent_entry.attr & 0x10))
        return -1;

    *out_parent_cluster = parent_cluster;

    return 0;
}

/* ========== VFS OPEN ДЛЯ FAT ========== */

int vfs_open_fat(const char *path, int flags)
{
    if (!path || !*path)
        return -1;

    if (path[0] == '/' &&
        path[1] == 'd' &&
        path[2] == 'e' &&
        path[3] == 'v')
    {
        return -1;
    }

    fat_dir_entry_t entry;
    uint32_t cluster;

    if (fat_lookup_path(
            &fatfs,
            path,
            &entry,
            &cluster
        ) != 0)
    {
        return -1;
    }

    int fd = alloc_fd();

    if (fd < 0)
        return -1;

    fat_private_t *priv =
        (fat_private_t*)kmalloc(
            sizeof(fat_private_t)
        );

    if (!priv)
        return -1;

    memset(priv,
           0,
           sizeof(fat_private_t));

    priv->cluster = cluster;

    memcpy(&priv->entry,
           &entry,
           sizeof(fat_dir_entry_t));

    priv->is_dir =
        (entry.attr & 0x10) ? 1 : 0;

    if (priv->is_dir) {

        if (fat_opendir(
                &fatfs,
                cluster,
                &priv->dir
            ) != 0)
        {
            kfree(priv);
            return -1;
        }
    }

    inode_t *inode =
        vfs_create_inode(
            cluster ? cluster : 1,
            priv->is_dir
                ? FT_DIRECTORY
                : FT_REGULAR,
            &fat_inode_ops,
            priv
        );

    if (!inode) {
        kfree(priv);
        return -1;
    }

    inode->size =
        read_le32(
            (const uint8_t*)&entry.file_size
        );

    file_t *f =
        alloc_file();

    if (!f) {
        kfree(inode);
        kfree(priv);
        return -1;
    }

    f->fd = fd;
    f->inode = inode;
    f->flags = flags;
    f->offset = 0;

    if (priv->is_dir)
        f->ops = &fat_dir_ops;
    else
        f->ops = &fat_file_ops;

    current_fd_table->files[fd] = f;
    current_fd_table->count++;

    printf("[VFS] Opened '%s' as fd=%d\n",
           path,
           fd);

    return fd;
}

static int fat_inode_readdir(inode_t *dir,
                             void *buf,
                             int index)
{
    (void)index;

    if (!dir ||
        !buf ||
        !dir->private_data)
    {
        return -1;
    }

    fat_private_t *priv =
        (fat_private_t*)dir->private_data;

    if (!priv->is_dir)
        return -1;

    fat_dir_entry_t entry;

    int result =
        fat_readdir(
            &priv->dir,
            &entry
        );

    if (result <= 0)
        return result;

    vfs_dirent_t *out =
        (vfs_dirent_t*)buf;

    /*
     * inode number.
     */
    uint16_t low =
        read_le16(
            (const uint8_t*)&entry.first_cluster_low
        );

    uint16_t high = 0;

    if (fatfs.fat_type == 32) {
        high =
            read_le16(
                (const uint8_t*)&entry.first_cluster_high
            );
    }

    uint32_t cluster =
        ((uint32_t)high << 16) | low;

    out->ino =
        cluster ? cluster : 1;

    out->type =
        (entry.attr & 0x10)
            ? FT_DIRECTORY
            : FT_REGULAR;

    /*
     * Преобразуем FAT 8.3 -> обычное имя.
     */
    int pos = 0;

    for (int i = 0;
         i < 8 && entry.name[i] != ' ';
         i++)
    {
        out->name[pos++] =
            (char)entry.name[i];
    }

    if (entry.name[8] != ' ') {

        out->name[pos++] = '.';

        for (int i = 8;
             i < 11 &&
             entry.name[i] != ' ';
             i++)
        {
            out->name[pos++] =
                (char)entry.name[i];
        }
    }

    out->name[pos] = '\0';

    return 1;
}

static inode_t *fat_inode_lookup(inode_t *dir,
                                 const char *name)
{
    if (!dir ||
        !name ||
        !*name ||
        !dir->private_data)
    {
        return NULL;
    }

    fat_private_t *parent =
        (fat_private_t*)dir->private_data;

    if (!parent->is_dir)
        return NULL;

    fat_dir_entry_t entry;

    if (fat_find_entry(
            &fatfs,
            parent->cluster,
            name,
            &entry
        ) != 0)
    {
        return NULL;
    }

    uint16_t low =
        read_le16(
            (const uint8_t*)&entry.first_cluster_low
        );

    uint16_t high = 0;

    if (fatfs.fat_type == 32) {
        high =
            read_le16(
                (const uint8_t*)&entry.first_cluster_high
            );
    }

    uint32_t cluster =
        ((uint32_t)high << 16) | low;

    fat_private_t *priv =
        (fat_private_t*)kmalloc(
            sizeof(fat_private_t)
        );

    if (!priv)
        return NULL;

    memset(priv,
           0,
           sizeof(fat_private_t));

    priv->cluster = cluster;

    memcpy(&priv->entry,
           &entry,
           sizeof(fat_dir_entry_t));

    priv->is_dir =
        (entry.attr & 0x10) ? 1 : 0;

    if (priv->is_dir) {

        fat_opendir(
            &fatfs,
            cluster,
            &priv->dir
        );
    }

    uint32_t ino =
        cluster ? cluster : 1;

    inode_t *inode =
        vfs_create_inode(
            ino,
            priv->is_dir
                ? FT_DIRECTORY
                : FT_REGULAR,
            &fat_inode_ops,
            priv
        );

    if (!inode) {
        kfree(priv);
        return NULL;
    }

    inode->size =
        read_le32(
            (const uint8_t*)&entry.file_size
        );

    return inode;
}

static int fat_inode_create(inode_t *dir,
                            const char *name,
                            file_type_t type)
{
    if (!dir ||
        !name ||
        !*name ||
        !dir->private_data)
    {
        return -1;
    }

    fat_private_t *priv =
        (fat_private_t*)dir->private_data;

    if (!priv->is_dir)
        return -1;

    if (type == FT_DIRECTORY) {

        return fat_mkdir(
            &fatfs,
            priv->cluster,
            name
        );
    }

    return fat_create_file(
        &fatfs,
        priv->cluster,
        name
    );
}

static int fat_inode_remove(inode_t *dir,
                            const char *name)
{
    if (!dir ||
        !name ||
        !*name ||
        !dir->private_data)
    {
        return -1;
    }

    fat_private_t *priv =
        (fat_private_t*)dir->private_data;

    if (!priv->is_dir)
        return -1;

    return fat_rm(
        &fatfs,
        priv->cluster,
        name
    );
}

int vfs_fat_create(const char *path)
{
    if (!path || !*path)
        return -1;

    uint32_t parent_cluster;
    char name[256];

    if (fat_resolve_parent(
            &fatfs,
            path,
            &parent_cluster,
            name
        ) != 0)
    {
        return -1;
    }

    /*
     * Уже существует?
     */
    fat_dir_entry_t existing;

    if (fat_find_entry(
            &fatfs,
            parent_cluster,
            name,
            &existing
        ) == 0)
    {
        return -2;
    }

    return fat_create_file(
        &fatfs,
        parent_cluster,
        name
    );
}

int vfs_fat_mkdir(const char *path)
{
    if (!path || !*path)
        return -1;

    /*
     * "/" нельзя создавать.
     */
    if (strcmp(path, "/") == 0)
        return -2;

    uint32_t parent_cluster;
    char name[256];

    if (fat_resolve_parent(
            &fatfs,
            path,
            &parent_cluster,
            name
        ) != 0)
    {
        return -1;
    }

    return fat_mkdir(
        &fatfs,
        parent_cluster,
        name
    );
}

int vfs_fat_unlink(const char *path)
{
    if (!path || !*path)
        return -1;

    if (strcmp(path, "/") == 0)
        return -2;

    uint32_t parent_cluster;
    char name[256];

    if (fat_resolve_parent(
            &fatfs,
            path,
            &parent_cluster,
            name
        ) != 0)
    {
        return -1;
    }

    return fat_rm(
        &fatfs,
        parent_cluster,
        name
    );
}

inode_t *vfs_fat_lookup(const char *path)
{
    if (!path || !*path)
        return NULL;

    /*
     * Root.
     */
    while (*path == '/')
        path++;

    if (!*path)
        return vfs_fat_get_root();

    fat_dir_entry_t entry;
    uint32_t cluster;

    if (fat_lookup_path(
            &fatfs,
            path,
            &entry,
            &cluster
        ) != 0)
    {
        return NULL;
    }

    fat_private_t *priv =
        (fat_private_t*)kmalloc(
            sizeof(fat_private_t)
        );

    if (!priv)
        return NULL;

    memset(priv,
           0,
           sizeof(fat_private_t));

    priv->cluster = cluster;

    memcpy(&priv->entry,
           &entry,
           sizeof(fat_dir_entry_t));

    priv->is_dir =
        (entry.attr & 0x10) ? 1 : 0;

    if (priv->is_dir) {

        if (fat_opendir(
                &fatfs,
                cluster,
                &priv->dir
            ) != 0)
        {
            kfree(priv);
            return NULL;
        }
    }

    inode_t *inode =
        vfs_create_inode(
            cluster ? cluster : 1,
            priv->is_dir
                ? FT_DIRECTORY
                : FT_REGULAR,
            &fat_inode_ops,
            priv
        );

    if (!inode) {
        kfree(priv);
        return NULL;
    }

    inode->size =
        read_le32(
            (const uint8_t*)&entry.file_size
        );

    return inode;
}

inode_t *vfs_fat_get_root(void)
{
    fat_private_t *priv =
        (fat_private_t*)kmalloc(
            sizeof(fat_private_t)
        );

    if (!priv)
        return NULL;

    memset(priv,
           0,
           sizeof(fat_private_t));

    /*
     * 0 = root в нашем VFS API.
     * fat_opendir сам подставит root_cluster
     * для FAT32.
     */
    priv->cluster = 0;
    priv->is_dir = 1;

    if (fat_opendir(
            &fatfs,
            0,
            &priv->dir
        ) != 0)
    {
        kfree(priv);
        return NULL;
    }

    fat_dir_entry_t fake_entry;
    memset(&fake_entry,
           0,
           sizeof(fake_entry));

    priv->entry.attr = 0x10;

    inode_t *inode =
        vfs_create_inode(
            1,
            FT_DIRECTORY,
            &fat_inode_ops,
            priv
        );

    if (!inode) {
        kfree(priv);
        return NULL;
    }

    inode->size = 0;

    return inode;
}