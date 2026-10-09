#include "ext2.h"
#include "system/mm/heap.h"
#include "lib/stddef.h"
#include "lib/string.h"

static inline void *blk_ptr(ext2_fs_t *fs, uint32_t block_num) {
    return fs->image + (uint64_t)block_num * fs->block_size;
}

static void mark_dirty(ext2_fs_t *fs, uint32_t byte_off, uint32_t len) {
    if (!fs->dirty_map || len == 0) return;
    uint32_t first = byte_off / 512;
    uint32_t last = (byte_off + len - 1) / 512;
    for (uint32_t lba = first; lba <= last && (lba >> 3) < fs->dirty_map_size; lba++)
        fs->dirty_map[lba >> 3] |= (1 << (lba & 7));
}

static inline ext2_inode_t *inode_ptr(ext2_fs_t *fs, uint32_t ino) {
    uint32_t group = (ino - 1) / fs->sb->s_inodes_per_group;
    uint32_t idx = (ino - 1) % fs->sb->s_inodes_per_group;
    uint32_t table_block = fs->groups[group].bg_inode_table;
    uint64_t off = (uint64_t)table_block * fs->block_size + (uint64_t)idx * fs->inode_size;
    return (ext2_inode_t *)(fs->image + off);
}

int ext2_init(ext2_fs_t *fs, uint8_t *image, uint32_t image_size) {
    memset(fs, 0, sizeof(*fs));
    if (image_size < 2048) return -1;

    fs->image = image;
    fs->image_size = image_size;
    fs->sb = (ext2_superblock_t *)(image + 1024);

    if (fs->sb->s_magic != EXT2_MAGIC) return -1;
    if (fs->sb->s_rev_level >= 1 &&
        (fs->sb->s_feature_incompat & (EXT2_FEATURE_INCOMPAT_EXTENTS | EXT2_FEATURE_INCOMPAT_64BIT)))
        return -1; // настоящий ext4 (extents/64bit) — не наш формат, см. ext2.h

    fs->block_size = 1024u << fs->sb->s_log_block_size;
    if (fs->block_size == 0 || (uint64_t)fs->block_size * 2 > image_size) return -1;
    // s_blocks_per_group/s_inodes_per_group — знаменатели ниже и в
    // inode_ptr()/alloc_block()/alloc_inode() — ноль у "магически
    // совпавшего" не-ext2 образа (например, auto-detect sys_mount()
    // пробующего ext2 на FAT/exFAT-флешке) привёл бы к #DE, тем же классом
    // бага, что был найден и исправлен в fat_init() (см. его комментарий).
    if (fs->sb->s_blocks_per_group == 0 || fs->sb->s_inodes_per_group == 0) return -1;

    fs->inode_size = (fs->sb->s_rev_level >= 1 && fs->sb->s_inode_size) ? fs->sb->s_inode_size : 128;
    fs->has_filetype = (fs->sb->s_rev_level >= 1) &&
                        (fs->sb->s_feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE);

    fs->groups_count = (fs->sb->s_blocks_count + fs->sb->s_blocks_per_group - 1) / fs->sb->s_blocks_per_group;

    uint32_t gdt_block = (fs->block_size == 1024) ? 2 : 1; // суперблок всегда по смещению 1024
    fs->groups = (ext2_group_desc_t *)blk_ptr(fs, gdt_block);

    uint32_t map_size = (image_size / 512 + 7) / 8;
    fs->dirty_map = (uint8_t *)kmalloc(map_size);
    if (!fs->dirty_map) return -1;
    memset(fs->dirty_map, 0, map_size);
    fs->dirty_map_size = map_size;

    return 0;
}

int ext2_read_inode(ext2_fs_t *fs, uint32_t ino, ext2_inode_t *out) {
    if (ino == 0 || ino > fs->sb->s_inodes_count) return -1;
    memcpy(out, inode_ptr(fs, ino), sizeof(ext2_inode_t));
    return 0;
}

/* ===================== Битовые карты / аллокация ===================== */

static int bitmap_alloc(ext2_fs_t *fs, uint32_t bitmap_block, uint32_t count_in_group) {
    uint8_t *bm = (uint8_t *)blk_ptr(fs, bitmap_block);
    for (uint32_t i = 0; i < count_in_group; i++) {
        if (!(bm[i >> 3] & (1 << (i & 7)))) {
            bm[i >> 3] |= (1 << (i & 7));
            mark_dirty(fs, (uint32_t)((uint8_t *)bm - fs->image) + (i >> 3), 1);
            return (int)i;
        }
    }
    return -1;
}

static void bitmap_free(ext2_fs_t *fs, uint32_t bitmap_block, uint32_t index_in_group) {
    uint8_t *bm = (uint8_t *)blk_ptr(fs, bitmap_block);
    bm[index_in_group >> 3] &= ~(1 << (index_in_group & 7));
    mark_dirty(fs, (uint32_t)((uint8_t *)bm - fs->image) + (index_in_group >> 3), 1);
}

// Выделяет один свободный блок данных (из первой группы, где есть место),
// зануляет его (новые блоки — всегда с нуля, не мусор из предыдущего
// использования) и обновляет free_blocks_count в группе/суперблоке.
// Возвращает номер блока или 0 (0 никогда не валидный номер блока данных).
static uint32_t alloc_block(ext2_fs_t *fs) {
    for (uint32_t g = 0; g < fs->groups_count; g++) {
        if (fs->groups[g].bg_free_blocks_count == 0) continue;
        int idx = bitmap_alloc(fs, fs->groups[g].bg_block_bitmap, fs->sb->s_blocks_per_group);
        if (idx < 0) continue;

        uint32_t block = fs->sb->s_first_data_block + g * fs->sb->s_blocks_per_group + (uint32_t)idx;
        memset(blk_ptr(fs, block), 0, fs->block_size);
        mark_dirty(fs, block * fs->block_size, fs->block_size);

        fs->groups[g].bg_free_blocks_count--;
        fs->sb->s_free_blocks_count--;
        mark_dirty(fs, (uint32_t)((uint8_t *)&fs->groups[g] - fs->image), sizeof(ext2_group_desc_t));
        mark_dirty(fs, 1024, sizeof(ext2_superblock_t));
        return block;
    }
    return 0;
}

static void free_block(ext2_fs_t *fs, uint32_t block) {
    if (!block) return;
    uint32_t g = (block - fs->sb->s_first_data_block) / fs->sb->s_blocks_per_group;
    uint32_t idx = (block - fs->sb->s_first_data_block) % fs->sb->s_blocks_per_group;
    bitmap_free(fs, fs->groups[g].bg_block_bitmap, idx);
    fs->groups[g].bg_free_blocks_count++;
    fs->sb->s_free_blocks_count++;
    mark_dirty(fs, (uint32_t)((uint8_t *)&fs->groups[g] - fs->image), sizeof(ext2_group_desc_t));
    mark_dirty(fs, 1024, sizeof(ext2_superblock_t));
}

static uint32_t alloc_inode(ext2_fs_t *fs, int is_dir) {
    for (uint32_t g = 0; g < fs->groups_count; g++) {
        if (fs->groups[g].bg_free_inodes_count == 0) continue;
        int idx = bitmap_alloc(fs, fs->groups[g].bg_inode_bitmap, fs->sb->s_inodes_per_group);
        if (idx < 0) continue;

        uint32_t ino = g * fs->sb->s_inodes_per_group + (uint32_t)idx + 1;
        memset(inode_ptr(fs, ino), 0, fs->inode_size);
        mark_dirty(fs, (uint32_t)((uint8_t *)inode_ptr(fs, ino) - fs->image), fs->inode_size);

        fs->groups[g].bg_free_inodes_count--;
        fs->sb->s_free_inodes_count--;
        if (is_dir) fs->groups[g].bg_used_dirs_count++;
        mark_dirty(fs, (uint32_t)((uint8_t *)&fs->groups[g] - fs->image), sizeof(ext2_group_desc_t));
        mark_dirty(fs, 1024, sizeof(ext2_superblock_t));
        return ino;
    }
    return 0;
}

static void free_inode(ext2_fs_t *fs, uint32_t ino, int is_dir) {
    uint32_t g = (ino - 1) / fs->sb->s_inodes_per_group;
    uint32_t idx = (ino - 1) % fs->sb->s_inodes_per_group;
    bitmap_free(fs, fs->groups[g].bg_inode_bitmap, idx);
    fs->groups[g].bg_free_inodes_count++;
    fs->sb->s_free_inodes_count++;
    if (is_dir && fs->groups[g].bg_used_dirs_count > 0) fs->groups[g].bg_used_dirs_count--;
    mark_dirty(fs, (uint32_t)((uint8_t *)&fs->groups[g] - fs->image), sizeof(ext2_group_desc_t));
    mark_dirty(fs, 1024, sizeof(ext2_superblock_t));
}

/* ====================== Отображение блоков файла ====================== */

// i_blocks считает 512-байтные СЕКТОРА (не ext2-блоки), занятые и данными,
// И самими косвенными таблицами-указателями — классическое поле ext2,
// которое проверяет e2fsck (найдено живым тестом: fsck ругался "i_blocks
// is 0, should be 2" на свежесозданных файлах, пока этого хелпера не было).
static inline void account_block(ext2_fs_t *fs, ext2_inode_t *inode) {
    inode->i_blocks += fs->block_size / 512;
    mark_dirty(fs, (uint32_t)((uint8_t *)&inode->i_blocks - fs->image), 4);
}

// Номер блока файла по индексу (0-based), с опциональным выделением по
// требованию (write-путь) недостающих прямых/косвенных блоков. Тройная
// косвенность не поддержана (см. ext2.h) — такой индекс возвращает 0 без
// выделения, вызывающий (ext2_read_file()/ext2_write_file()) это как ошибку.
static uint32_t block_for_index(ext2_fs_t *fs, ext2_inode_t *inode, uint32_t index, int allocate) {
    uint32_t ptrs_per_block = fs->block_size / 4;

    if (index < 12) {
        if (inode->i_block[index] == 0 && allocate) {
            inode->i_block[index] = alloc_block(fs);
            if (inode->i_block[index]) account_block(fs, inode);
        }
        return inode->i_block[index];
    }
    index -= 12;

    if (index < ptrs_per_block) {
        if (inode->i_block[12] == 0) {
            if (!allocate) return 0;
            inode->i_block[12] = alloc_block(fs);
            if (!inode->i_block[12]) return 0;
            account_block(fs, inode);
        }
        uint32_t *tbl = (uint32_t *)blk_ptr(fs, inode->i_block[12]);
        if (tbl[index] == 0 && allocate) {
            tbl[index] = alloc_block(fs);
            mark_dirty(fs, (uint32_t)((uint8_t *)&tbl[index] - fs->image), 4);
            if (tbl[index]) account_block(fs, inode);
        }
        return tbl[index];
    }
    index -= ptrs_per_block;

    if (index < ptrs_per_block * ptrs_per_block) {
        if (inode->i_block[13] == 0) {
            if (!allocate) return 0;
            inode->i_block[13] = alloc_block(fs);
            if (!inode->i_block[13]) return 0;
            account_block(fs, inode);
        }
        uint32_t outer_i = index / ptrs_per_block;
        uint32_t inner_i = index % ptrs_per_block;
        uint32_t *outer = (uint32_t *)blk_ptr(fs, inode->i_block[13]);
        if (outer[outer_i] == 0) {
            if (!allocate) return 0;
            outer[outer_i] = alloc_block(fs);
            mark_dirty(fs, (uint32_t)((uint8_t *)&outer[outer_i] - fs->image), 4);
            if (!outer[outer_i]) return 0;
            account_block(fs, inode);
        }
        uint32_t *inner = (uint32_t *)blk_ptr(fs, outer[outer_i]);
        if (inner[inner_i] == 0 && allocate) {
            inner[inner_i] = alloc_block(fs);
            mark_dirty(fs, (uint32_t)((uint8_t *)&inner[inner_i] - fs->image), 4);
            if (inner[inner_i]) account_block(fs, inode);
        }
        return inner[inner_i];
    }

    return 0; // тройная косвенность — не поддержано
}

int ext2_read_file(ext2_fs_t *fs, uint32_t ino, uint32_t offset, void *buf, uint32_t count) {
    ext2_inode_t inode;
    if (ext2_read_inode(fs, ino, &inode) != 0) return -1;
    if (offset >= inode.i_size) return 0;
    if (offset + count > inode.i_size) count = inode.i_size - offset;

    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < count) {
        uint32_t file_off = offset + done;
        uint32_t idx = file_off / fs->block_size;
        uint32_t in_block_off = file_off % fs->block_size;
        uint32_t chunk = fs->block_size - in_block_off;
        if (chunk > count - done) chunk = count - done;

        uint32_t block = block_for_index(fs, &inode, idx, 0);
        if (block == 0) {
            memset(out + done, 0, chunk); // "дыра" (sparse file) — читается нулями
        } else {
            memcpy(out + done, (uint8_t *)blk_ptr(fs, block) + in_block_off, chunk);
        }
        done += chunk;
    }
    return (int)done;
}

int ext2_write_file(ext2_fs_t *fs, uint32_t ino, uint32_t offset, const void *buf, uint32_t count) {
    ext2_inode_t *inode = inode_ptr(fs, ino);
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t done = 0;

    while (done < count) {
        uint32_t file_off = offset + done;
        uint32_t idx = file_off / fs->block_size;
        uint32_t in_block_off = file_off % fs->block_size;
        uint32_t chunk = fs->block_size - in_block_off;
        if (chunk > count - done) chunk = count - done;

        uint32_t block = block_for_index(fs, inode, idx, 1);
        if (block == 0) return done > 0 ? (int)done : -1; // нет свободного места

        memcpy((uint8_t *)blk_ptr(fs, block) + in_block_off, in + done, chunk);
        mark_dirty(fs, block * fs->block_size + in_block_off, chunk);
        done += chunk;
    }

    if (offset + done > inode->i_size) {
        inode->i_size = offset + done;
        mark_dirty(fs, (uint32_t)((uint8_t *)&inode->i_size - fs->image), 4);
    }
    return (int)done;
}

/* ========================= Directory entries ========================= */

static int dirent_min_len(int name_len) {
    return (8 + name_len + 3) & ~3; // заголовок (8) + имя, выровнено до 4 байт
}

// Обходит ВСЕ data-блоки каталога, зовя cb(ctx, dirent_ptr, block, byte_off_in_block)
// для каждой записи с inode != 0; cb возвращает 1, чтобы остановить обход
// (нашли то, что искали) — тогда walk тоже возвращает 1. 0 — дошли до
// конца без находки, -1 — структурная ошибка.
typedef int (*dirent_cb_t)(void *ctx, ext2_dirent_t *de, uint32_t block, uint32_t off_in_block);

static int walk_dirents(ext2_fs_t *fs, ext2_inode_t *dir, dirent_cb_t cb, void *ctx) {
    uint32_t nblocks = (dir->i_size + fs->block_size - 1) / fs->block_size;
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t block = block_for_index(fs, dir, bi, 0);
        if (block == 0) continue;
        uint32_t off = 0;
        while (off < fs->block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)((uint8_t *)blk_ptr(fs, block) + off);
            if (de->rec_len < 8) break; // повреждённая структура — не крутимся вечно
            if (de->inode != 0) {
                int r = cb(ctx, de, block, off);
                if (r) return r;
            }
            off += de->rec_len;
        }
    }
    return 0;
}

typedef struct { const char *name; int name_len; uint32_t found_ino; int found_is_dir; } find_ctx_t;

static int find_cb(void *vctx, ext2_dirent_t *de, uint32_t block, uint32_t off) {
    (void)block; (void)off;
    find_ctx_t *ctx = (find_ctx_t *)vctx;
    if (de->name_len != ctx->name_len) return 0;
    const char *name = (const char *)(de + 1);
    for (int i = 0; i < ctx->name_len; i++) if (name[i] != ctx->name[i]) return 0;
    ctx->found_ino = de->inode;
    ctx->found_is_dir = (de->file_type == 2);
    return 1;
}

static int find_in_dir(ext2_fs_t *fs, uint32_t dir_ino, const char *name, int name_len, uint32_t *out_ino) {
    ext2_inode_t dir;
    if (ext2_read_inode(fs, dir_ino, &dir) != 0 || !ext2_is_dir(&dir)) return -1;
    find_ctx_t ctx = { name, name_len, 0, 0 };
    if (!walk_dirents(fs, &dir, find_cb, &ctx)) return -1;
    *out_ino = ctx.found_ino;
    return 0;
}

int ext2_lookup_path(ext2_fs_t *fs, const char *path, uint32_t *out_ino) {
    uint32_t cur = EXT2_ROOT_INO;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        int len = 0;
        const char *comp = p;
        while (comp[len] && comp[len] != '/') len++;
        uint32_t next;
        if (find_in_dir(fs, cur, comp, len, &next) != 0 || next == 0) return -1;
        cur = next;
        p = comp + len;
        while (*p == '/') p++;
    }
    *out_ino = cur;
    return 0;
}

int ext2_resolve_parent(ext2_fs_t *fs, const char *path, uint32_t *out_parent_ino, char *leaf_out, int leaf_cap) {
    const char *p = path;
    while (*p == '/') p++;
    if (!*p) return -1; // сам корень — не имеет родителя в этом контексте

    uint32_t cur = EXT2_ROOT_INO;
    for (;;) {
        int len = 0;
        const char *comp = p;
        while (comp[len] && comp[len] != '/') len++;
        const char *rest = comp + len;
        while (*rest == '/') rest++;

        if (!*rest) {
            *out_parent_ino = cur;
            int n = (len < leaf_cap - 1) ? len : leaf_cap - 1;
            memcpy(leaf_out, comp, n);
            leaf_out[n] = '\0';
            return 0;
        }

        uint32_t next;
        if (find_in_dir(fs, cur, comp, len, &next) != 0 || next == 0) return -1;
        cur = next;
        p = rest;
    }
}

typedef struct { int index; int cur; char *name_out; uint32_t *ino_out; int *is_dir_out; int found; } readdir_ctx_t;

static int readdir_cb(void *vctx, ext2_dirent_t *de, uint32_t block, uint32_t off) {
    (void)block; (void)off;
    readdir_ctx_t *ctx = (readdir_ctx_t *)vctx;
    // "."/".." пропускаем — как и FAT/RAMFS readdir в этом ядре, список не включает их.
    if (de->name_len == 1 && ((const char *)(de + 1))[0] == '.') return 0;
    if (de->name_len == 2 && ((const char *)(de + 1))[0] == '.' && ((const char *)(de + 1))[1] == '.') return 0;

    if (ctx->cur == ctx->index) {
        int n = de->name_len < EXT2_NAME_LEN ? de->name_len : EXT2_NAME_LEN;
        memcpy(ctx->name_out, (const char *)(de + 1), n);
        ctx->name_out[n] = '\0';
        *ctx->ino_out = de->inode;
        *ctx->is_dir_out = (de->file_type == 2);
        ctx->found = 1;
        return 1;
    }
    ctx->cur++;
    return 0;
}

int ext2_readdir(ext2_fs_t *fs, uint32_t dir_ino, int index, char *name_out, uint32_t *ino_out, int *is_dir_out) {
    ext2_inode_t dir;
    if (ext2_read_inode(fs, dir_ino, &dir) != 0 || !ext2_is_dir(&dir)) return -1;
    readdir_ctx_t ctx = { index, 0, name_out, ino_out, is_dir_out, 0 };
    walk_dirents(fs, &dir, readdir_cb, &ctx);
    return ctx.found ? 1 : 0;
}

// Ищет место под новую запись (name_len байт имени) среди уже существующих
// dirent'ов каталога, расщепляя первый подходящий по свободному "хвосту"
// rec_len; если такого места нет — выделяет НОВЫЙ блок данных каталога
// (целиком свободная запись на весь блок) и пишет в его начало. 0 при успехе.
static int insert_dirent(ext2_fs_t *fs, uint32_t dir_ino, ext2_inode_t *dir, uint32_t new_ino,
                          const char *name, int name_len, int file_type) {
    int need = dirent_min_len(name_len);
    uint32_t nblocks = (dir->i_size + fs->block_size - 1) / fs->block_size;

    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t block = block_for_index(fs, dir, bi, 0);
        if (block == 0) continue;
        uint32_t off = 0;
        while (off < fs->block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)((uint8_t *)blk_ptr(fs, block) + off);
            if (de->rec_len < 8) break;
            int used = (de->inode != 0) ? dirent_min_len(de->name_len) : 0;
            int free_tail = de->rec_len - used;

            if (free_tail >= need) {
                uint16_t old_rec_len = de->rec_len;
                if (de->inode != 0) {
                    de->rec_len = (uint16_t)used;
                    ext2_dirent_t *nde = (ext2_dirent_t *)((uint8_t *)de + used);
                    nde->inode = new_ino;
                    nde->rec_len = (uint16_t)(old_rec_len - used);
                    nde->name_len = (uint8_t)name_len;
                    nde->file_type = fs->has_filetype ? (uint8_t)file_type : 0;
                    memcpy((uint8_t *)(nde + 1), name, name_len);
                    mark_dirty(fs, (uint32_t)((uint8_t *)de - fs->image), old_rec_len);
                } else {
                    de->inode = new_ino;
                    de->name_len = (uint8_t)name_len;
                    de->file_type = fs->has_filetype ? (uint8_t)file_type : 0;
                    memcpy((uint8_t *)(de + 1), name, name_len);
                    mark_dirty(fs, (uint32_t)((uint8_t *)de - fs->image), old_rec_len);
                }
                (void)dir_ino;
                return 0;
            }
            off += de->rec_len;
        }
    }

    // Не нашлось места — новый блок, растим каталог.
    uint32_t new_block = block_for_index(fs, dir, nblocks, 1);
    if (new_block == 0) return -1;
    dir->i_size += fs->block_size;
    mark_dirty(fs, (uint32_t)((uint8_t *)&dir->i_size - fs->image), 4);

    ext2_dirent_t *de = (ext2_dirent_t *)blk_ptr(fs, new_block);
    de->inode = new_ino;
    de->rec_len = (uint16_t)fs->block_size;
    de->name_len = (uint8_t)name_len;
    de->file_type = fs->has_filetype ? (uint8_t)file_type : 0;
    memcpy((uint8_t *)(de + 1), name, name_len);
    mark_dirty(fs, new_block * fs->block_size, fs->block_size);
    return 0;
}

int ext2_create(ext2_fs_t *fs, uint32_t parent_ino, const char *name, uint32_t *out_ino) {
    int name_len = (int)strlen(name);
    if (name_len == 0 || name_len > EXT2_NAME_LEN) return -1;

    uint32_t existing;
    if (find_in_dir(fs, parent_ino, name, name_len, &existing) == 0 && existing != 0) return -1;

    ext2_inode_t *parent = inode_ptr(fs, parent_ino);
    if (!ext2_is_dir(parent)) return -1;

    uint32_t ino = alloc_inode(fs, 0);
    if (!ino) return -1;
    ext2_inode_t *inode = inode_ptr(fs, ino);
    inode->i_mode = EXT2_S_IFREG | 0644;
    inode->i_links_count = 1;
    mark_dirty(fs, (uint32_t)((uint8_t *)inode - fs->image), fs->inode_size);

    if (insert_dirent(fs, parent_ino, parent, ino, name, name_len, 1) != 0) {
        free_inode(fs, ino, 0);
        return -1;
    }
    *out_ino = ino;
    return 0;
}

int ext2_mkdir(ext2_fs_t *fs, uint32_t parent_ino, const char *name, uint32_t *out_ino) {
    int name_len = (int)strlen(name);
    if (name_len == 0 || name_len > EXT2_NAME_LEN) return -1;

    uint32_t existing;
    if (find_in_dir(fs, parent_ino, name, name_len, &existing) == 0 && existing != 0) return -1;

    ext2_inode_t *parent = inode_ptr(fs, parent_ino);
    if (!ext2_is_dir(parent)) return -1;

    uint32_t ino = alloc_inode(fs, 1);
    if (!ino) return -1;
    ext2_inode_t *inode = inode_ptr(fs, ino);
    inode->i_mode = EXT2_S_IFDIR | 0755;
    inode->i_links_count = 2; // "." указывает сам на себя + запись в родителе
    mark_dirty(fs, (uint32_t)((uint8_t *)inode - fs->image), fs->inode_size);

    uint32_t block = block_for_index(fs, inode, 0, 1);
    if (!block) { free_inode(fs, ino, 1); return -1; }
    inode->i_size = fs->block_size;
    mark_dirty(fs, (uint32_t)((uint8_t *)&inode->i_size - fs->image), 4);

    ext2_dirent_t *dot = (ext2_dirent_t *)blk_ptr(fs, block);
    dot->inode = ino; dot->rec_len = 12; dot->name_len = 1; dot->file_type = fs->has_filetype ? 2 : 0;
    ((char *)(dot + 1))[0] = '.';

    ext2_dirent_t *dotdot = (ext2_dirent_t *)((uint8_t *)blk_ptr(fs, block) + 12);
    dotdot->inode = parent_ino; dotdot->rec_len = (uint16_t)(fs->block_size - 12);
    dotdot->name_len = 2; dotdot->file_type = fs->has_filetype ? 2 : 0;
    ((char *)(dotdot + 1))[0] = '.'; ((char *)(dotdot + 1))[1] = '.';
    mark_dirty(fs, block * fs->block_size, fs->block_size);

    if (insert_dirent(fs, parent_ino, parent, ino, name, name_len, 2) != 0) {
        free_block(fs, block);
        free_inode(fs, ino, 1);
        return -1;
    }
    parent->i_links_count++; // ".." нового каталога ссылается на родителя
    mark_dirty(fs, (uint32_t)((uint8_t *)&parent->i_links_count - fs->image), 2);

    *out_ino = ino;
    return 0;
}

// true, если каталог содержит только "."/".." (т.е. безопасно удалять).
static int dir_is_empty(ext2_fs_t *fs, ext2_inode_t *dir) {
    readdir_ctx_t ctx = { 0, 0, NULL, NULL, NULL, 0 };
    // Переиспользуем readdir_cb логику без записи вывода: если найдётся
    // хоть одна НЕ "."/".."-запись, walk_dirents остановится (cb вернёт 1
    // через readdir_ctx с index=0, cur=0 → found сразу), иначе дойдёт до конца.
    char dummy_name[EXT2_NAME_LEN + 1];
    uint32_t dummy_ino; int dummy_is_dir;
    ctx.name_out = dummy_name; ctx.ino_out = &dummy_ino; ctx.is_dir_out = &dummy_is_dir;
    walk_dirents(fs, dir, readdir_cb, &ctx);
    return !ctx.found;
}

static void free_all_blocks(ext2_fs_t *fs, ext2_inode_t *inode) {
    uint32_t ptrs_per_block = fs->block_size / 4;
    for (int i = 0; i < 12; i++) if (inode->i_block[i]) free_block(fs, inode->i_block[i]);

    if (inode->i_block[12]) {
        uint32_t *tbl = (uint32_t *)blk_ptr(fs, inode->i_block[12]);
        for (uint32_t i = 0; i < ptrs_per_block; i++) if (tbl[i]) free_block(fs, tbl[i]);
        free_block(fs, inode->i_block[12]);
    }
    if (inode->i_block[13]) {
        uint32_t *outer = (uint32_t *)blk_ptr(fs, inode->i_block[13]);
        for (uint32_t i = 0; i < ptrs_per_block; i++) {
            if (!outer[i]) continue;
            uint32_t *inner = (uint32_t *)blk_ptr(fs, outer[i]);
            for (uint32_t j = 0; j < ptrs_per_block; j++) if (inner[j]) free_block(fs, inner[j]);
            free_block(fs, outer[i]);
        }
        free_block(fs, inode->i_block[13]);
    }
}

void ext2_truncate(ext2_fs_t *fs, uint32_t ino) {
    ext2_inode_t *inode = inode_ptr(fs, ino);
    free_all_blocks(fs, inode);
    memset(inode->i_block, 0, sizeof(inode->i_block));
    inode->i_size = 0;
    inode->i_blocks = 0;
    mark_dirty(fs, (uint32_t)((uint8_t *)inode - fs->image), fs->inode_size);
}

int ext2_unlink(ext2_fs_t *fs, uint32_t parent_ino, const char *name) {
    int name_len = (int)strlen(name);
    ext2_inode_t *parent = inode_ptr(fs, parent_ino);
    if (!ext2_is_dir(parent)) return -1;

    uint32_t nblocks = (parent->i_size + fs->block_size - 1) / fs->block_size;
    ext2_dirent_t *prev_in_block = NULL;
    uint32_t target_ino = 0;
    int target_is_dir = 0;

    for (uint32_t bi = 0; bi < nblocks && target_ino == 0; bi++) {
        uint32_t block = block_for_index(fs, parent, bi, 0);
        if (block == 0) continue;
        uint32_t off = 0;
        prev_in_block = NULL;
        while (off < fs->block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)((uint8_t *)blk_ptr(fs, block) + off);
            if (de->rec_len < 8) break;
            if (de->inode != 0 && de->name_len == name_len) {
                const char *dn = (const char *)(de + 1);
                int match = 1;
                for (int i = 0; i < name_len; i++) if (dn[i] != name[i]) { match = 0; break; }
                if (match) {
                    target_ino = de->inode;
                    target_is_dir = (de->file_type == 2);
                    if (prev_in_block) {
                        prev_in_block->rec_len += de->rec_len;
                        mark_dirty(fs, (uint32_t)((uint8_t *)prev_in_block - fs->image), prev_in_block->rec_len);
                    } else {
                        de->inode = 0; // первая запись в блоке — просто гасим, rec_len остаётся
                        mark_dirty(fs, (uint32_t)((uint8_t *)de - fs->image), de->rec_len);
                    }
                    break;
                }
            }
            prev_in_block = de;
            off += de->rec_len;
        }
    }

    if (target_ino == 0) return -1;

    ext2_inode_t *target = inode_ptr(fs, target_ino);
    if (target_is_dir) {
        if (!dir_is_empty(fs, target)) return -1;
        parent->i_links_count--; // ушла ".." удаляемого каталога
        mark_dirty(fs, (uint32_t)((uint8_t *)&parent->i_links_count - fs->image), 2);
        free_all_blocks(fs, target);
        free_inode(fs, target_ino, 1);
    } else {
        if (target->i_links_count > 0) target->i_links_count--;
        if (target->i_links_count == 0) {
            free_all_blocks(fs, target);
            free_inode(fs, target_ino, 0);
        } else {
            mark_dirty(fs, (uint32_t)((uint8_t *)&target->i_links_count - fs->image), 2);
        }
    }
    return 0;
}
