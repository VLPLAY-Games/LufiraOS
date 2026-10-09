// RAMFS — см. разбор архитектуры и границ в ramfs.h. В отличие от
// fat_mount.c, узлы — настоящее дерево (kmalloc на узел, связный список
// детей), а не байты плоского образа устройства, поэтому вложенные
// каталоги поддерживаются в принципе, без искусственного "только корневой
// уровень".
#include "ramfs.h"
#include "../vfs/vfs.h"
#include "system/mm/heap.h"
#include "lib/stddef.h"
#include "lib/string.h"

typedef struct ramfs_node {
    char name[RAMFS_MAX_NAME];
    file_type_t type;
    uint8_t *data;              // NULL для каталогов
    uint32_t size;
    uint32_t cap;
    struct ramfs_node *parent;
    struct ramfs_node *children; // голова связного списка детей (каталоги)
    struct ramfs_node *next;     // указатель на следующего "брата"
} ramfs_node_t;

typedef struct {
    int in_use;
    char prefix[RAMFS_MOUNT_PREFIX_MAX];
    ramfs_node_t *root;
} vfs_ramfs_mount_t;

static vfs_ramfs_mount_t g_mounts[RAMFS_MAX_MOUNTS];

static ramfs_node_t *node_alloc(const char *name, file_type_t type, ramfs_node_t *parent) {
    ramfs_node_t *n = (ramfs_node_t *)kmalloc(sizeof(*n));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    int i = 0;
    while (name[i] && i < RAMFS_MAX_NAME - 1) { n->name[i] = name[i]; i++; }
    n->name[i] = '\0';
    n->type = type;
    n->parent = parent;
    return n;
}

static ramfs_node_t *find_child(ramfs_node_t *dir, const char *name, int name_len) {
    for (ramfs_node_t *c = dir->children; c; c = c->next) {
        int i = 0;
        while (i < name_len && c->name[i] == name[i]) i++;
        if (i == name_len && c->name[i] == '\0') return c;
    }
    return NULL;
}

// Возвращает указатель на следующий компонент пути и его длину в *len_out
// (0, если p указывает только на '/'-ы или на конец строки — т.е. путь
// исчерпан). Пропускает ведущие '/', как и повторяющиеся разделители
// между компонентами ("a//b" эквивалентно "a/b").
static const char *next_component(const char *p, int *len_out) {
    while (*p == '/') p++;
    int n = 0;
    while (p[n] && p[n] != '/') n++;
    *len_out = n;
    return p;
}

// Полное разрешение rel (без ведущего '/', уже снятого вызывающим) в узел,
// начиная от root. NULL, если какой-либо промежуточный компонент не
// существует или не каталог.
static ramfs_node_t *resolve(ramfs_node_t *root, const char *rel) {
    ramfs_node_t *cur = root;
    const char *p = rel;
    for (;;) {
        int len;
        const char *comp = next_component(p, &len);
        if (len == 0) return cur; // путь исчерпан — текущий узел и есть результат
        if (cur->type != FT_DIRECTORY) return NULL;
        ramfs_node_t *child = find_child(cur, comp, len);
        if (!child) return NULL;
        cur = child;
        p = comp + len;
    }
}

// Разрешает rel в (родительский каталог, имя последнего компонента) — для
// create/mkdir/unlink, где последний компонент сам ещё может не
// существовать, а все остальные обязаны существовать и быть каталогами.
// rel не может быть пустым (пустой rel — это сам корень монтирования,
// обрабатывается отдельно вызывающим). 0 при успехе, -1 иначе.
static int resolve_parent(ramfs_node_t *root, const char *rel,
                           ramfs_node_t **parent_out, char *leaf_out, int leaf_cap) {
    ramfs_node_t *cur = root;
    const char *p = rel;
    for (;;) {
        int len;
        const char *comp = next_component(p, &len);
        if (len == 0) return -1;

        const char *rest = comp + len;
        const char *peek = rest;
        while (*peek == '/') peek++;

        if (!*peek) {
            // comp — последний компонент (leaf)
            if (cur->type != FT_DIRECTORY) return -1;
            *parent_out = cur;
            int n = (len < leaf_cap - 1) ? len : leaf_cap - 1;
            memcpy(leaf_out, comp, n);
            leaf_out[n] = '\0';
            return 0;
        }

        if (cur->type != FT_DIRECTORY) return -1;
        ramfs_node_t *child = find_child(cur, comp, len);
        if (!child || child->type != FT_DIRECTORY) return -1;
        cur = child;
        p = rest;
    }
}

static void free_tree(ramfs_node_t *n) {
    ramfs_node_t *c = n->children;
    while (c) {
        ramfs_node_t *next = c->next;
        free_tree(c);
        c = next;
    }
    if (n->data) kfree(n->data);
    kfree(n);
}

static vfs_ramfs_mount_t *find_mount_for_path(const char *path, char *rel_out, int rel_cap) {
    for (int i = 0; i < RAMFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].in_use) continue;
        vfs_ramfs_mount_t *m = &g_mounts[i];
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

int vfs_ramfs_mount_count(void) {
    int n = 0;
    for (int i = 0; i < RAMFS_MAX_MOUNTS; i++) if (g_mounts[i].in_use) n++;
    return n;
}

int ramfs_mount(const char *prefix) {
    if (!prefix || prefix[0] != '/') return -1;

    for (int i = 0; i < RAMFS_MAX_MOUNTS; i++) {
        if (g_mounts[i].in_use && strcmp(g_mounts[i].prefix, prefix) == 0) return -2;
    }

    int slot = -1;
    for (int i = 0; i < RAMFS_MAX_MOUNTS; i++) {
        if (!g_mounts[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -3;

    ramfs_node_t *root = node_alloc("", FT_DIRECTORY, NULL);
    if (!root) return -4;

    vfs_ramfs_mount_t *m = &g_mounts[slot];
    int n = 0;
    while (prefix[n] && n < RAMFS_MOUNT_PREFIX_MAX - 1) { m->prefix[n] = prefix[n]; n++; }
    m->prefix[n] = '\0';
    m->root = root;
    m->in_use = 1;
    return 0;
}

int ramfs_unmount(const char *prefix) {
    for (int i = 0; i < RAMFS_MAX_MOUNTS; i++) {
        if (g_mounts[i].in_use && strcmp(g_mounts[i].prefix, prefix) == 0) {
            free_tree(g_mounts[i].root);
            g_mounts[i].root = NULL;
            g_mounts[i].in_use = 0;
            return 0;
        }
    }
    return -1;
}

/* ========== file_ops (обычные файлы) ========== */

static int ramfs_file_read(file_t *f, void *buf, size_t count) {
    if (!f || !f->inode || !f->inode->private_data) return -1;
    ramfs_node_t *n = (ramfs_node_t *)f->inode->private_data;

    if (f->offset >= n->size) return 0;
    uint32_t avail = n->size - f->offset;
    uint32_t c = (count < avail) ? (uint32_t)count : avail;
    memcpy(buf, n->data + f->offset, c);
    f->offset += c;
    return (int)c;
}

static int ramfs_file_write(file_t *f, const void *buf, size_t count) {
    if (!f || !f->inode || !f->inode->private_data) return -1;
    ramfs_node_t *n = (ramfs_node_t *)f->inode->private_data;

    uint32_t offset = (f->flags & O_APPEND) ? n->size : f->offset;
    uint32_t end = offset + (uint32_t)count;

    if (end > n->cap) {
        uint8_t *nb = (uint8_t *)kmalloc(end);
        if (!nb) return -1;
        if (n->data) memcpy(nb, n->data, n->size);
        if (n->data) kfree(n->data);
        n->data = nb;
        n->cap = end;
    }

    memcpy(n->data + offset, buf, count);
    if (end > n->size) n->size = end;
    f->offset = end;
    f->inode->size = n->size;
    return (int)count;
}

static int ramfs_file_seek(file_t *f, off_t offset, int whence) {
    if (!f || !f->inode || !f->inode->private_data) return -1;
    ramfs_node_t *n = (ramfs_node_t *)f->inode->private_data;

    switch (whence) {
        case SEEK_SET: f->offset = (uint32_t)offset; break;
        case SEEK_CUR: f->offset += (uint32_t)offset; break;
        case SEEK_END: f->offset = n->size + (uint32_t)offset; break;
        default: return -1;
    }
    return (int)f->offset;
}

// Нарочно пустая close() — в отличие от fat_mount.c, inode->private_data
// тут указывает на ПОСТОЯННЫЙ узел дерева ramfs, а не на одноразовую
// копию файла под конкретный open(): его освобождает только
// vfs_ramfs_unlink()/ramfs_unmount(), не каждый vfs_close().
static int ramfs_file_close(file_t *f) {
    (void)f;
    return 0;
}

static file_ops_t ramfs_file_ops = {
    .read = ramfs_file_read,
    .write = ramfs_file_write,
    .seek = ramfs_file_seek,
    .close = ramfs_file_close,
};

/* ========== inode_ops (каталоги — любой, не только корень монтирования) */

static int ramfs_dir_readdir(inode_t *dir, void *buf, int index) {
    if (!dir || !buf || !dir->private_data) return -1;
    ramfs_node_t *dn = (ramfs_node_t *)dir->private_data;

    ramfs_node_t *c = dn->children;
    int i = 0;
    while (c && i < index) { c = c->next; i++; }
    if (!c) return 0; // конец

    vfs_dirent_t *out = (vfs_dirent_t *)buf;
    out->ino = 0;
    out->type = c->type;
    strcpy(out->name, c->name);
    return 1;
}

static inode_ops_t ramfs_dir_ops = {
    .lookup = NULL,
    .create = NULL,
    .remove = NULL,
    .readdir = ramfs_dir_readdir,
};

/* ========== path-level API (см. ramfs.h) ========== */

int vfs_ramfs_open(const char *path, int flags) {
    char rel[256];
    vfs_ramfs_mount_t *m = find_mount_for_path(path, rel, sizeof(rel));
    if (!m) return -2;

    ramfs_node_t *node = (rel[0] == '\0') ? m->root : resolve(m->root, rel);

    if (!node) {
        if (!(flags & O_CREAT)) return -1;

        ramfs_node_t *parent;
        char leaf[RAMFS_MAX_NAME];
        if (resolve_parent(m->root, rel, &parent, leaf, sizeof(leaf)) != 0) return -1;
        if (find_child(parent, leaf, (int)strlen(leaf))) return -1;

        node = node_alloc(leaf, FT_REGULAR, parent);
        if (!node) return -1;
        node->next = parent->children;
        parent->children = node;
    }

    int fd = alloc_fd();
    if (fd < 0) return -1;

    file_t *f = alloc_file();
    if (!f) return -1;

    if (node->type == FT_DIRECTORY) {
        inode_t *inode = vfs_create_inode(0, FT_DIRECTORY, &ramfs_dir_ops, node);
        if (!inode) return -1;
        f->fd = fd;
        f->inode = inode;
        f->offset = 0;
        f->flags = flags;
        f->ops = NULL; // читают через vfs_readdir()/f->inode->ops, не f->ops
    } else {
        if (flags & O_TRUNC) {
            node->size = 0;
        }
        inode_t *inode = vfs_create_inode(0, FT_REGULAR, NULL, node);
        if (!inode) return -1;
        inode->size = node->size;
        f->fd = fd;
        f->inode = inode;
        f->offset = (flags & O_APPEND) ? node->size : 0;
        f->flags = flags;
        f->ops = &ramfs_file_ops;
    }

    current_fd_table->files[fd] = f;
    current_fd_table->count++;
    return fd;
}

int vfs_ramfs_mkdir(const char *path) {
    char rel[256];
    vfs_ramfs_mount_t *m = find_mount_for_path(path, rel, sizeof(rel));
    if (!m) return -2;
    if (rel[0] == '\0') return -1; // сам корень монтирования уже существует

    ramfs_node_t *parent;
    char leaf[RAMFS_MAX_NAME];
    if (resolve_parent(m->root, rel, &parent, leaf, sizeof(leaf)) != 0) return -1;
    if (find_child(parent, leaf, (int)strlen(leaf))) return -1; // уже существует

    ramfs_node_t *n = node_alloc(leaf, FT_DIRECTORY, parent);
    if (!n) return -1;
    n->next = parent->children;
    parent->children = n;
    return 0;
}

// Общая для unlink() и rmdir() на уровне vfs.c (та же конвенция, что
// vfs_fat_unlink() — см. fat_mount.c и вызовы в vfs.c): удаляет и файл, и
// ПУСТОЙ каталог; непустой каталог отклоняется.
int vfs_ramfs_unlink(const char *path) {
    char rel[256];
    vfs_ramfs_mount_t *m = find_mount_for_path(path, rel, sizeof(rel));
    if (!m) return -2;
    if (rel[0] == '\0') return -1; // нельзя удалить сам корень монтирования

    ramfs_node_t *parent;
    char leaf[RAMFS_MAX_NAME];
    if (resolve_parent(m->root, rel, &parent, leaf, sizeof(leaf)) != 0) return -1;

    ramfs_node_t *target = find_child(parent, leaf, (int)strlen(leaf));
    if (!target) return -1;
    if (target->type == FT_DIRECTORY && target->children) return -1; // не пусто

    if (parent->children == target) {
        parent->children = target->next;
    } else {
        ramfs_node_t *p = parent->children;
        while (p && p->next != target) p = p->next;
        if (p) p->next = target->next;
    }

    if (target->data) kfree(target->data);
    kfree(target);
    return 0;
}

inode_t *vfs_ramfs_lookup(const char *path) {
    char rel[256];
    vfs_ramfs_mount_t *m = find_mount_for_path(path, rel, sizeof(rel));
    if (!m) return NULL;

    ramfs_node_t *node = (rel[0] == '\0') ? m->root : resolve(m->root, rel);
    if (!node) return NULL;

    inode_t *inode = vfs_create_inode(0, node->type,
                                       (node->type == FT_DIRECTORY) ? &ramfs_dir_ops : NULL,
                                       node);
    if (inode) inode->size = node->size;
    return inode;
}
