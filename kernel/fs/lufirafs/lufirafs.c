// Драйвер LufiraFS — собственная файловая система ядра, независимая от FAT
// (см. формат диска в lufirafs_format.h). Персистентность как у FAT: весь
// диск загружен в RAM единым куском (fs->image), запись отслеживается
// битовой картой "грязных" блоков и сбрасывается на диск через drivers/disk
// в lufirafs_sync()/lufirafs_flush().
#include "lufirafs.h"
#include "system/mm/pmm.h"
#include "system/mm/heap.h"
#include "drivers/disk/disk.h"
#include "drivers/console/console.h"
#include "system/devmode/devmode.h"
#include "lib/stddef.h"
#include "lib/string.h"

int lufirafs_mounted = 0;

static inline uint8_t* get_block_ptr(lufirafs_t *fs, uint32_t block) {
    return fs->image + (uint64_t)block * LUFIRAFS_BLOCK_SIZE;
}

static void mark_dirty(lufirafs_t *fs, uint32_t block) {
    if (block >= fs->sb.total_blocks) return;
    fs->dirty_bitmap[block / 8] |= (uint8_t)(1u << (block % 8));
}

// Суперблок живёт целиком в блоке 0 — при любом изменении полей sb
// переписываем блок 0 из fs->sb и помечаем его грязным.
static void write_superblock(lufirafs_t *fs) {
    memcpy(fs->image, &fs->sb, sizeof(fs->sb));
    mark_dirty(fs, 0);
}

// ===== Битовая карта блоков данных =====

static inline uint8_t* block_bitmap_ptr(lufirafs_t *fs) {
    return fs->image + (uint64_t)fs->sb.bitmap_start * LUFIRAFS_BLOCK_SIZE;
}

static int block_bitmap_test(lufirafs_t *fs, uint32_t block) {
    uint8_t *bm = block_bitmap_ptr(fs);
    return (bm[block / 8] >> (block % 8)) & 1;
}

static void block_bitmap_set(lufirafs_t *fs, uint32_t block, int used) {
    uint8_t *bm = block_bitmap_ptr(fs);
    if (used) bm[block / 8] |= (uint8_t)(1u << (block % 8));
    else      bm[block / 8] &= (uint8_t)~(1u << (block % 8));

    uint32_t bitmap_block = fs->sb.bitmap_start + (block / 8) / LUFIRAFS_BLOCK_SIZE;
    mark_dirty(fs, bitmap_block);
}

static uint32_t alloc_block(lufirafs_t *fs) {
    for (uint32_t b = fs->sb.data_start; b < fs->sb.total_blocks; b++) {
        if (!block_bitmap_test(fs, b)) {
            block_bitmap_set(fs, b, 1);
            memset(get_block_ptr(fs, b), 0, LUFIRAFS_BLOCK_SIZE);
            mark_dirty(fs, b);
            if (fs->sb.free_blocks > 0) fs->sb.free_blocks--;
            write_superblock(fs);
            return b;
        }
    }
    return 0;
}

static void free_data_block(lufirafs_t *fs, uint32_t block) {
    if (block == 0) return;
    block_bitmap_set(fs, block, 0);
    fs->sb.free_blocks++;
    write_superblock(fs);
}

// ===== Таблица inode =====

int lufirafs_read_inode(lufirafs_t *fs, uint32_t ino, lufirafs_inode_t *out) {
    if (!fs || !out || ino == 0 || ino > fs->sb.inode_count) return -1;
    uint32_t byte_off = fs->sb.inode_table_start * LUFIRAFS_BLOCK_SIZE
                       + (ino - 1) * LUFIRAFS_INODE_SIZE;
    memcpy(out, fs->image + byte_off, sizeof(lufirafs_inode_t));
    return 0;
}

int lufirafs_write_inode(lufirafs_t *fs, uint32_t ino, const lufirafs_inode_t *in) {
    if (!fs || !in || ino == 0 || ino > fs->sb.inode_count) return -1;
    uint32_t byte_off = fs->sb.inode_table_start * LUFIRAFS_BLOCK_SIZE
                       + (ino - 1) * LUFIRAFS_INODE_SIZE;
    memcpy(fs->image + byte_off, in, sizeof(lufirafs_inode_t));
    mark_dirty(fs, byte_off / LUFIRAFS_BLOCK_SIZE);
    return 0;
}

static uint32_t alloc_inode(lufirafs_t *fs) {
    for (uint32_t ino = 1; ino <= fs->sb.inode_count; ino++) {
        lufirafs_inode_t tmp;
        lufirafs_read_inode(fs, ino, &tmp);
        if (tmp.mode == LUFIRAFS_MODE_FREE) {
            if (fs->sb.free_inodes > 0) fs->sb.free_inodes--;
            write_superblock(fs);
            return ino;
        }
    }
    return 0;
}

static void free_inode_slot(lufirafs_t *fs, uint32_t ino) {
    lufirafs_inode_t empty;
    memset(&empty, 0, sizeof(empty));
    lufirafs_write_inode(fs, ino, &empty);
    fs->sb.free_inodes++;
    write_superblock(fs);
}

// Возвращает номер block_index-го блока данных inode (0-based). create=1 —
// при отсутствии выделяет новый блок (и, если нужно, косвенный блок) и
// прописывает указатель на него в inode/косвенном блоке. inode передаётся
// и читается, и (при create) пишется вызывающей стороной отдельно.
static uint32_t inode_get_block(lufirafs_t *fs, lufirafs_inode_t *inode, uint32_t block_index, int create) {
    if (block_index < LUFIRAFS_DIRECT_BLOCKS) {
        if (inode->direct[block_index] == 0 && create) {
            uint32_t nb = alloc_block(fs);
            if (!nb) return 0;
            inode->direct[block_index] = nb;
        }
        return inode->direct[block_index];
    }

    uint32_t idx = block_index - LUFIRAFS_DIRECT_BLOCKS;
    if (idx >= LUFIRAFS_PTRS_PER_BLOCK) return 0; // за пределами макс. размера файла

    if (inode->indirect == 0) {
        if (!create) return 0;
        uint32_t ib = alloc_block(fs);
        if (!ib) return 0;
        inode->indirect = ib;
    }

    uint32_t *iptrs = (uint32_t*)get_block_ptr(fs, inode->indirect);
    if (iptrs[idx] == 0 && create) {
        uint32_t nb = alloc_block(fs);
        if (!nb) return 0;
        iptrs[idx] = nb;
        mark_dirty(fs, inode->indirect);
    }
    return iptrs[idx];
}

// ===== Копирование ограниченного по длине имени (strncpy в lib/string.h нет) =====

static void copy_name(char *dest, const char *src, int max_len) {
    int i = 0;
    while (i < max_len && src[i]) { dest[i] = src[i]; i++; }
    dest[i] = '\0';
}

// ===== Каталоги =====

static int lookup_in_dir(lufirafs_t *fs, uint32_t dir_ino, const char *name, uint32_t *out_ino) {
    lufirafs_inode_t dir;
    if (lufirafs_read_inode(fs, dir_ino, &dir) != 0) return -1;
    if (dir.mode != LUFIRAFS_MODE_DIR) return -1;

    uint32_t nblocks = (dir.size + LUFIRAFS_BLOCK_SIZE - 1) / LUFIRAFS_BLOCK_SIZE;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t bn = inode_get_block(fs, &dir, b, 0);
        if (!bn) continue;
        lufirafs_dirent_t *ents = (lufirafs_dirent_t*)get_block_ptr(fs, bn);
        for (uint32_t i = 0; i < LUFIRAFS_DIRENTS_PER_BLOCK; i++) {
            if (ents[i].inode != 0 && strcmp(ents[i].name, name) == 0) {
                *out_ino = ents[i].inode;
                return 0;
            }
        }
    }
    return -1;
}

static int add_dirent(lufirafs_t *fs, uint32_t dir_ino, const char *name, uint32_t entry_ino) {
    lufirafs_inode_t dir;
    if (lufirafs_read_inode(fs, dir_ino, &dir) != 0) return -1;

    uint32_t nblocks = (dir.size + LUFIRAFS_BLOCK_SIZE - 1) / LUFIRAFS_BLOCK_SIZE;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t bn = inode_get_block(fs, &dir, b, 0);
        if (!bn) continue;
        lufirafs_dirent_t *ents = (lufirafs_dirent_t*)get_block_ptr(fs, bn);
        for (uint32_t i = 0; i < LUFIRAFS_DIRENTS_PER_BLOCK; i++) {
            if (ents[i].inode == 0) {
                ents[i].inode = entry_ino;
                copy_name(ents[i].name, name, LUFIRAFS_MAX_NAME);
                mark_dirty(fs, bn);
                return 0;
            }
        }
    }

    // Свободного слота нет — растим каталог на один блок.
    uint32_t bn = inode_get_block(fs, &dir, nblocks, 1);
    if (!bn) return -1;

    lufirafs_dirent_t *ents = (lufirafs_dirent_t*)get_block_ptr(fs, bn);
    ents[0].inode = entry_ino;
    copy_name(ents[0].name, name, LUFIRAFS_MAX_NAME);
    mark_dirty(fs, bn);

    dir.size = (nblocks + 1) * LUFIRAFS_BLOCK_SIZE;
    lufirafs_write_inode(fs, dir_ino, &dir);
    return 0;
}

static int remove_dirent(lufirafs_t *fs, uint32_t dir_ino, const char *name) {
    lufirafs_inode_t dir;
    if (lufirafs_read_inode(fs, dir_ino, &dir) != 0) return -1;

    uint32_t nblocks = (dir.size + LUFIRAFS_BLOCK_SIZE - 1) / LUFIRAFS_BLOCK_SIZE;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t bn = inode_get_block(fs, &dir, b, 0);
        if (!bn) continue;
        lufirafs_dirent_t *ents = (lufirafs_dirent_t*)get_block_ptr(fs, bn);
        for (uint32_t i = 0; i < LUFIRAFS_DIRENTS_PER_BLOCK; i++) {
            if (ents[i].inode != 0 && strcmp(ents[i].name, name) == 0) {
                ents[i].inode = 0;
                ents[i].name[0] = '\0';
                mark_dirty(fs, bn);
                return 0;
            }
        }
    }
    return -1;
}

int lufirafs_opendir(lufirafs_t *fs, uint32_t ino, lufirafs_dir_t *dir) {
    if (!fs || !dir) return -1;
    dir->fs = fs;
    dir->dir_ino = ino;
    dir->index = 0;
    return 0;
}

int lufirafs_readdir(lufirafs_dir_t *dir, lufirafs_dirent_t *out) {
    if (!dir || !out) return -1;
    lufirafs_t *fs = dir->fs;

    lufirafs_inode_t inode;
    if (lufirafs_read_inode(fs, dir->dir_ino, &inode) != 0) return -1;

    uint32_t total_entries =
        ((inode.size + LUFIRAFS_BLOCK_SIZE - 1) / LUFIRAFS_BLOCK_SIZE) * LUFIRAFS_DIRENTS_PER_BLOCK;

    while (dir->index < total_entries) {
        uint32_t block_index = dir->index / LUFIRAFS_DIRENTS_PER_BLOCK;
        uint32_t entry_index = dir->index % LUFIRAFS_DIRENTS_PER_BLOCK;
        dir->index++;

        uint32_t bn = inode_get_block(fs, &inode, block_index, 0);
        if (!bn) continue;

        lufirafs_dirent_t *ents = (lufirafs_dirent_t*)get_block_ptr(fs, bn);
        if (ents[entry_index].inode != 0) {
            *out = ents[entry_index];
            return 0;
        }
    }
    return -1;
}

// ===== Разрешение путей =====

int lufirafs_lookup(lufirafs_t *fs, uint32_t start_inode, const char *path, uint32_t *out_inode) {
    if (!fs || !path || !out_inode) return -1;

    uint32_t cur = (path[0] == '/') ? fs->sb.root_inode : start_inode;
    const char *p = path;
    if (*p == '/') p++;

    if (*p == '\0') { // путь был просто "/" (или пустой после отсечения)
        *out_inode = cur;
        return 0;
    }

    char component[LUFIRAFS_MAX_NAME + 1];
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < LUFIRAFS_MAX_NAME) component[i++] = *p++;
        component[i] = '\0';
        while (*p == '/') p++;
        if (i == 0) continue;

        uint32_t next;
        if (lookup_in_dir(fs, cur, component, &next) != 0) return -1;
        cur = next;
    }

    *out_inode = cur;
    return 0;
}

int lufirafs_resolve_parent(lufirafs_t *fs, uint32_t start_inode, const char *path,
                             uint32_t *out_parent, char *out_name) {
    if (!fs || !path || !*path || !out_parent || !out_name) return -1;

    int len = (int)strlen(path);
    while (len > 1 && path[len - 1] == '/') len--; // отбрасываем хвостовые '/'
    if (len == 0) return -1;

    int last_slash = -1;
    for (int i = 0; i < len; i++) if (path[i] == '/') last_slash = i;

    if (last_slash == -1) {
        // просто имя без слэшей — родитель это start_inode
        if (len > LUFIRAFS_MAX_NAME) return -1;
        copy_name(out_name, path, len);
        *out_parent = start_inode;
        return 0;
    }

    const char *name = path + last_slash + 1;
    int name_len = len - (last_slash + 1);
    if (name_len <= 0 || name_len > LUFIRAFS_MAX_NAME) return -1;
    copy_name(out_name, name, name_len);

    if (last_slash == 0) {
        // "/name" — родитель корень
        *out_parent = fs->sb.root_inode;
        return 0;
    }

    char dir_path[256];
    if (last_slash >= (int)sizeof(dir_path)) return -1;
    memcpy(dir_path, path, (size_t)last_slash);
    dir_path[last_slash] = '\0';

    return lufirafs_lookup(fs, start_inode, dir_path, out_parent);
}

// ===== Создание / удаление =====

int lufirafs_create(lufirafs_t *fs, uint32_t parent_ino, const char *name, uint32_t mode, uint32_t *out_ino) {
    if (!fs || !name || !*name || !out_ino) return -1;

    uint32_t existing;
    if (lookup_in_dir(fs, parent_ino, name, &existing) == 0) return -1; // уже существует

    uint32_t ino = alloc_inode(fs);
    if (!ino) return -2;

    lufirafs_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.mode = mode;
    inode.links_count = 1;
    lufirafs_write_inode(fs, ino, &inode);

    if (mode == LUFIRAFS_MODE_DIR) {
        if (add_dirent(fs, ino, ".", ino) != 0 ||
            add_dirent(fs, ino, "..", parent_ino) != 0) {
            free_inode_slot(fs, ino);
            return -3;
        }
        inode.links_count = 2;
        lufirafs_write_inode(fs, ino, &inode);
    }

    if (add_dirent(fs, parent_ino, name, ino) != 0) {
        free_inode_slot(fs, ino);
        return -3;
    }

    *out_ino = ino;
    return 0;
}

int lufirafs_unlink(lufirafs_t *fs, uint32_t parent_ino, const char *name) {
    if (!fs || !name || !*name) return -1;

    uint32_t ino;
    if (lookup_in_dir(fs, parent_ino, name, &ino) != 0) return -1;
    if (ino == fs->sb.root_inode) return -3;

    lufirafs_inode_t inode;
    if (lufirafs_read_inode(fs, ino, &inode) != 0) return -1;

    if (inode.mode == LUFIRAFS_MODE_DIR) {
        lufirafs_dir_t dir;
        lufirafs_opendir(fs, ino, &dir);
        lufirafs_dirent_t ent;
        int extra = 0;
        while (lufirafs_readdir(&dir, &ent) == 0) {
            if (strcmp(ent.name, ".") != 0 && strcmp(ent.name, "..") != 0) extra++;
        }
        if (extra > 0) return -2;
    }

    uint32_t nblocks = (inode.size + LUFIRAFS_BLOCK_SIZE - 1) / LUFIRAFS_BLOCK_SIZE;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t bn = inode_get_block(fs, &inode, b, 0);
        if (bn) free_data_block(fs, bn);
    }
    if (inode.indirect) free_data_block(fs, inode.indirect);

    free_inode_slot(fs, ino);
    remove_dirent(fs, parent_ino, name);
    return 0;
}

// ===== Чтение / запись содержимого файла =====

int lufirafs_read(lufirafs_t *fs, uint32_t ino, uint32_t offset, void *buf, uint32_t count) {
    if (!fs || !buf) return -1;

    lufirafs_inode_t inode;
    if (lufirafs_read_inode(fs, ino, &inode) != 0) return -1;
    if (offset >= inode.size) return 0;
    if (offset + count > inode.size) count = inode.size - offset;

    uint32_t remaining = count;
    uint32_t pos = offset;
    uint8_t *dst = (uint8_t*)buf;

    while (remaining > 0) {
        uint32_t block_index = pos / LUFIRAFS_BLOCK_SIZE;
        uint32_t block_off = pos % LUFIRAFS_BLOCK_SIZE;
        uint32_t chunk = LUFIRAFS_BLOCK_SIZE - block_off;
        if (chunk > remaining) chunk = remaining;

        uint32_t bn = inode_get_block(fs, &inode, block_index, 0);
        if (bn == 0) memset(dst, 0, chunk);
        else memcpy(dst, get_block_ptr(fs, bn) + block_off, chunk);

        dst += chunk;
        pos += chunk;
        remaining -= chunk;
    }
    return (int)count;
}

int lufirafs_write(lufirafs_t *fs, uint32_t ino, uint32_t offset, const void *buf, uint32_t count) {
    if (!fs || !buf) return -1;

    lufirafs_inode_t inode;
    if (lufirafs_read_inode(fs, ino, &inode) != 0) return -1;
    if ((uint64_t)offset + count > LUFIRAFS_MAX_FILE_SIZE) return -1;

    uint32_t remaining = count;
    uint32_t pos = offset;
    const uint8_t *src = (const uint8_t*)buf;

    while (remaining > 0) {
        uint32_t block_index = pos / LUFIRAFS_BLOCK_SIZE;
        uint32_t block_off = pos % LUFIRAFS_BLOCK_SIZE;
        uint32_t chunk = LUFIRAFS_BLOCK_SIZE - block_off;
        if (chunk > remaining) chunk = remaining;

        uint32_t bn = inode_get_block(fs, &inode, block_index, 1);
        if (bn == 0) {
            // Нет места — сохраняем то, что успели, обновляем размер и выходим.
            uint32_t written = count - remaining;
            if (offset + written > inode.size) inode.size = offset + written;
            lufirafs_write_inode(fs, ino, &inode);
            return written > 0 ? (int)written : -1;
        }

        memcpy(get_block_ptr(fs, bn) + block_off, src, chunk);
        mark_dirty(fs, bn);

        src += chunk;
        pos += chunk;
        remaining -= chunk;
    }

    if (pos > inode.size) inode.size = pos;
    lufirafs_write_inode(fs, ino, &inode);
    return (int)count;
}

int lufirafs_truncate(lufirafs_t *fs, uint32_t ino, uint32_t new_size) {
    if (!fs) return -1;

    lufirafs_inode_t inode;
    if (lufirafs_read_inode(fs, ino, &inode) != 0) return -1;

    if (new_size < inode.size) {
        uint32_t old_nblocks = (inode.size + LUFIRAFS_BLOCK_SIZE - 1) / LUFIRAFS_BLOCK_SIZE;
        uint32_t new_nblocks = (new_size + LUFIRAFS_BLOCK_SIZE - 1) / LUFIRAFS_BLOCK_SIZE;

        for (uint32_t b = new_nblocks; b < old_nblocks; b++) {
            uint32_t bn = inode_get_block(fs, &inode, b, 0);
            if (!bn) continue;
            free_data_block(fs, bn);
            if (b < LUFIRAFS_DIRECT_BLOCKS) {
                inode.direct[b] = 0;
            } else if (inode.indirect) {
                uint32_t *iptrs = (uint32_t*)get_block_ptr(fs, inode.indirect);
                iptrs[b - LUFIRAFS_DIRECT_BLOCKS] = 0;
                mark_dirty(fs, inode.indirect);
            }
        }

        if (new_nblocks == 0 && inode.indirect) {
            free_data_block(fs, inode.indirect);
            inode.indirect = 0;
        }
    }

    inode.size = new_size;
    lufirafs_write_inode(fs, ino, &inode);
    return 0;
}

uint32_t lufirafs_du_blocks(lufirafs_t *fs, uint32_t ino) {
    lufirafs_inode_t inode;
    if (lufirafs_read_inode(fs, ino, &inode) != 0) return 0;

    uint32_t blocks = (inode.size + LUFIRAFS_BLOCK_SIZE - 1) / LUFIRAFS_BLOCK_SIZE;
    if (inode.indirect) blocks += 1;

    if (inode.mode != LUFIRAFS_MODE_DIR) return blocks;

    uint32_t total = blocks;
    lufirafs_dir_t dir;
    lufirafs_opendir(fs, ino, &dir);
    lufirafs_dirent_t ent;
    while (lufirafs_readdir(&dir, &ent) == 0) {
        if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0) continue;
        total += lufirafs_du_blocks(fs, ent.inode);
    }
    return total;
}

// ===== Инициализация и синхронизация с диском =====

int lufirafs_init(lufirafs_t *fs, void *image, uint32_t image_size, uint32_t lba_offset) {
    if (!fs || !image) return -1;

    fs->image = (uint8_t*)image;
    fs->image_size = image_size;
    fs->lba_offset = lba_offset;

    memcpy(&fs->sb, fs->image, sizeof(fs->sb));

    if (fs->sb.magic != LUFIRAFS_MAGIC) {
        printf("[LufiraFS] Bad magic 0x%lx (ожидался 0x%lx) — диск не отформатирован\n",
               (uint64_t)fs->sb.magic, (uint64_t)LUFIRAFS_MAGIC);
        return -1;
    }
    if (fs->sb.block_size != LUFIRAFS_BLOCK_SIZE) {
        printf("[LufiraFS] Несовместимый размер блока на диске\n");
        return -1;
    }

    uint32_t dirty_bytes = (fs->sb.total_blocks + 7) / 8;
    fs->dirty_bitmap = (uint8_t*)kmalloc(dirty_bytes);
    if (!fs->dirty_bitmap) return -1;
    memset(fs->dirty_bitmap, 0, dirty_bytes);

    lufirafs_mounted = 1;
    return 0;
}

void lufirafs_sync(lufirafs_t *fs) {
    if (!fs || !fs->dirty_bitmap) return;

    uint32_t sectors_per_block = LUFIRAFS_BLOCK_SIZE / 512;
    uint32_t written = 0;

    for (uint32_t b = 0; b < fs->sb.total_blocks; b++) {
        if (fs->dirty_bitmap[b / 8] & (1u << (b % 8))) {
            uint32_t lba = fs->lba_offset + b * sectors_per_block;
            if (disk_write_sectors(lba, (uint8_t)sectors_per_block, get_block_ptr(fs, b)) == 0) {
                fs->dirty_bitmap[b / 8] &= (uint8_t)~(1u << (b % 8));
                written++;
            }
        }
    }

    if (written > 0) {
        DLOG("[LufiraFS] Synced %u block(s) to disk\n", written);
    }
}

void lufirafs_flush(lufirafs_t *fs) {
    lufirafs_sync(fs);
}
