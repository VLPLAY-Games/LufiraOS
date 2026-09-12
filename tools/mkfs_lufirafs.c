// mkfs_lufirafs — хостовый инструмент форматирования и наполнения региона
// диска в формате LufiraFS (см. kernel/fs/lufirafs/lufirafs_format.h,
// единственный источник истины для геометрии/структур — эта программа
// подключает ЕГО ЖЕ, а не свою копию, чтобы формат гарантированно совпадал
// с тем, что понимает ядро).
//
// Работает как обычная хостовая программа (полная libc) поверх файла-образа
// диска: читает нужный РЕГИОН (offset..offset+size) целиком в память,
// мутирует его тем же алгоритмом, что и kernel/fs/lufirafs/lufirafs.c
// (сознательно продублирован здесь в свободном от freestanding-ограничений
// виде — общий код между hosted/freestanding сборками того не стоит), и
// записывает регион обратно.
//
// Использование:
//   mkfs_lufirafs format <image> <offset> <size>
//   mkfs_lufirafs mkdir  <image> <offset> <size> </dest/path>
//   mkfs_lufirafs put    <image> <offset> <size> <host_file> </dest/path>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../kernel/fs/lufirafs/lufirafs_format.h"

static uint8_t *g_image;      // буфер РЕГИОНА (size байт)
static uint32_t g_size;
static lufirafs_superblock_t g_sb;

static uint8_t *block_ptr(uint32_t block) {
    return g_image + (uint64_t)block * LUFIRAFS_BLOCK_SIZE;
}

static void write_superblock(void) {
    memcpy(g_image, &g_sb, sizeof(g_sb));
}

static uint8_t *bitmap_ptr(void) {
    return g_image + (uint64_t)g_sb.bitmap_start * LUFIRAFS_BLOCK_SIZE;
}

static int bitmap_test(uint32_t block) {
    uint8_t *bm = bitmap_ptr();
    return (bm[block / 8] >> (block % 8)) & 1;
}

static void bitmap_set(uint32_t block, int used) {
    uint8_t *bm = bitmap_ptr();
    if (used) bm[block / 8] |= (uint8_t)(1u << (block % 8));
    else      bm[block / 8] &= (uint8_t)~(1u << (block % 8));
}

static uint32_t alloc_block(void) {
    for (uint32_t b = g_sb.data_start; b < g_sb.total_blocks; b++) {
        if (!bitmap_test(b)) {
            bitmap_set(b, 1);
            memset(block_ptr(b), 0, LUFIRAFS_BLOCK_SIZE);
            if (g_sb.free_blocks > 0) g_sb.free_blocks--;
            return b;
        }
    }
    fprintf(stderr, "mkfs_lufirafs: out of data blocks\n");
    exit(1);
}

static void read_inode(uint32_t ino, lufirafs_inode_t *out) {
    uint32_t off = g_sb.inode_table_start * LUFIRAFS_BLOCK_SIZE + (ino - 1) * LUFIRAFS_INODE_SIZE;
    memcpy(out, g_image + off, sizeof(*out));
}

static void write_inode(uint32_t ino, const lufirafs_inode_t *in) {
    uint32_t off = g_sb.inode_table_start * LUFIRAFS_BLOCK_SIZE + (ino - 1) * LUFIRAFS_INODE_SIZE;
    memcpy(g_image + off, in, sizeof(*in));
}

static uint32_t alloc_inode(void) {
    for (uint32_t ino = 1; ino <= g_sb.inode_count; ino++) {
        lufirafs_inode_t tmp;
        read_inode(ino, &tmp);
        if (tmp.mode == LUFIRAFS_MODE_FREE) {
            if (g_sb.free_inodes > 0) g_sb.free_inodes--;
            return ino;
        }
    }
    fprintf(stderr, "mkfs_lufirafs: out of inodes\n");
    exit(1);
}

static uint32_t inode_get_block(lufirafs_inode_t *inode, uint32_t block_index, int create) {
    if (block_index < LUFIRAFS_DIRECT_BLOCKS) {
        if (inode->direct[block_index] == 0 && create) {
            inode->direct[block_index] = alloc_block();
        }
        return inode->direct[block_index];
    }
    uint32_t idx = block_index - LUFIRAFS_DIRECT_BLOCKS;
    if (idx >= LUFIRAFS_PTRS_PER_BLOCK) {
        fprintf(stderr, "mkfs_lufirafs: file too large\n");
        exit(1);
    }
    if (inode->indirect == 0) {
        if (!create) return 0;
        inode->indirect = alloc_block();
    }
    uint32_t *iptrs = (uint32_t*)block_ptr(inode->indirect);
    if (iptrs[idx] == 0 && create) {
        iptrs[idx] = alloc_block();
    }
    return iptrs[idx];
}

static void add_dirent(uint32_t dir_ino, const char *name, uint32_t entry_ino) {
    lufirafs_inode_t dir;
    read_inode(dir_ino, &dir);

    uint32_t nblocks = (dir.size + LUFIRAFS_BLOCK_SIZE - 1) / LUFIRAFS_BLOCK_SIZE;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t bn = inode_get_block(&dir, b, 0);
        lufirafs_dirent_t *ents = (lufirafs_dirent_t*)block_ptr(bn);
        for (uint32_t i = 0; i < LUFIRAFS_DIRENTS_PER_BLOCK; i++) {
            if (ents[i].inode == 0) {
                ents[i].inode = entry_ino;
                strncpy(ents[i].name, name, LUFIRAFS_MAX_NAME);
                ents[i].name[LUFIRAFS_MAX_NAME] = '\0';
                return;
            }
        }
    }

    uint32_t bn = inode_get_block(&dir, nblocks, 1);
    lufirafs_dirent_t *ents = (lufirafs_dirent_t*)block_ptr(bn);
    ents[0].inode = entry_ino;
    strncpy(ents[0].name, name, LUFIRAFS_MAX_NAME);
    ents[0].name[LUFIRAFS_MAX_NAME] = '\0';

    dir.size = (nblocks + 1) * LUFIRAFS_BLOCK_SIZE;
    write_inode(dir_ino, &dir);
}

static int lookup_in_dir(uint32_t dir_ino, const char *name, uint32_t *out_ino) {
    lufirafs_inode_t dir;
    read_inode(dir_ino, &dir);
    if (dir.mode != LUFIRAFS_MODE_DIR) return -1;

    uint32_t nblocks = (dir.size + LUFIRAFS_BLOCK_SIZE - 1) / LUFIRAFS_BLOCK_SIZE;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t bn = inode_get_block(&dir, b, 0);
        if (!bn) continue;
        lufirafs_dirent_t *ents = (lufirafs_dirent_t*)block_ptr(bn);
        for (uint32_t i = 0; i < LUFIRAFS_DIRENTS_PER_BLOCK; i++) {
            if (ents[i].inode != 0 && strcmp(ents[i].name, name) == 0) {
                *out_ino = ents[i].inode;
                return 0;
            }
        }
    }
    return -1;
}

// Создаёт (или возвращает уже существующий) каталог по абсолютному пути,
// создавая недостающие промежуточные компоненты — как "mkdir -p".
static uint32_t ensure_dir_path(const char *path) {
    uint32_t cur = g_sb.root_inode;
    if (path[0] != '/') { fprintf(stderr, "mkfs_lufirafs: path must be absolute: %s\n", path); exit(1); }

    char component[LUFIRAFS_MAX_NAME + 1];
    const char *p = path + 1;
    while (*p) {
        int i = 0;
        while (*p && *p != '/' && i < LUFIRAFS_MAX_NAME) component[i++] = *p++;
        component[i] = '\0';
        while (*p == '/') p++;
        if (i == 0) continue;

        uint32_t next;
        if (lookup_in_dir(cur, component, &next) == 0) {
            cur = next;
            continue;
        }

        uint32_t new_ino = alloc_inode();
        lufirafs_inode_t inode;
        memset(&inode, 0, sizeof(inode));
        inode.mode = LUFIRAFS_MODE_DIR;
        inode.links_count = 2;
        write_inode(new_ino, &inode);
        add_dirent(new_ino, ".", new_ino);
        add_dirent(new_ino, "..", cur);
        add_dirent(cur, component, new_ino);
        cur = new_ino;
    }
    return cur;
}

// Разбивает абсолютный путь на (родительский каталог, имя последнего
// компонента), создавая промежуточные каталоги при необходимости.
static void split_path(const char *path, uint32_t *out_parent, char *out_name) {
    const char *last_slash = strrchr(path, '/');
    if (!last_slash) { fprintf(stderr, "mkfs_lufirafs: path must be absolute: %s\n", path); exit(1); }

    strncpy(out_name, last_slash + 1, LUFIRAFS_MAX_NAME);
    out_name[LUFIRAFS_MAX_NAME] = '\0';

    if (last_slash == path) {
        *out_parent = g_sb.root_inode;
        return;
    }

    char dir_path[512];
    size_t dlen = (size_t)(last_slash - path);
    if (dlen >= sizeof(dir_path)) { fprintf(stderr, "mkfs_lufirafs: path too long\n"); exit(1); }
    memcpy(dir_path, path, dlen);
    dir_path[dlen] = '\0';

    *out_parent = ensure_dir_path(dir_path);
}

static void cmd_format(void) {
    memset(g_image, 0, g_size);

    uint32_t total_blocks = g_size / LUFIRAFS_BLOCK_SIZE;
    lufirafs_compute_layout(total_blocks, &g_sb);
    if (g_sb.data_start >= total_blocks) {
        fprintf(stderr, "mkfs_lufirafs: region too small for superblock+bitmap+inodes\n");
        exit(1);
    }

    // Помечаем служебные блоки (суперблок + битовая карта + таблица inode)
    // занятыми, чтобы аллокатор их никогда не выдал под данные.
    for (uint32_t b = 0; b < g_sb.data_start; b++) {
        bitmap_set(b, 1);
    }
    g_sb.free_blocks = total_blocks - g_sb.data_start;
    g_sb.free_inodes = g_sb.inode_count;
    write_superblock();

    // Корневой каталог — inode LUFIRAFS_ROOT_INODE, "." и ".." указывают
    // сами на себя (у корня нет родителя).
    uint32_t root = g_sb.root_inode;
    lufirafs_inode_t root_inode;
    memset(&root_inode, 0, sizeof(root_inode));
    root_inode.mode = LUFIRAFS_MODE_DIR;
    root_inode.links_count = 2;
    write_inode(root, &root_inode);

    // free_inodes должен учитывать root ДО его аллокации в обычном смысле —
    // root не проходит через alloc_inode(), поэтому корректируем вручную.
    g_sb.free_inodes--;
    write_superblock();

    add_dirent(root, ".", root);
    add_dirent(root, "..", root);

    printf("mkfs_lufirafs: formatted %u blocks (%u bytes/block), %u inodes, data starts at block %u\n",
           g_sb.total_blocks, g_sb.block_size, g_sb.inode_count, g_sb.data_start);
}

static void cmd_mkdir(const char *path) {
    ensure_dir_path(path);
}

static void cmd_put(const char *host_file, const char *dest_path) {
    FILE *f = fopen(host_file, "rb");
    if (!f) { fprintf(stderr, "mkfs_lufirafs: cannot open %s\n", host_file); exit(1); }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 0) { fprintf(stderr, "mkfs_lufirafs: ftell failed on %s\n", host_file); exit(1); }

    uint8_t *data = (uint8_t*)malloc((size_t)fsize > 0 ? (size_t)fsize : 1);
    if (fsize > 0 && fread(data, 1, (size_t)fsize, f) != (size_t)fsize) {
        fprintf(stderr, "mkfs_lufirafs: short read on %s\n", host_file);
        exit(1);
    }
    fclose(f);

    uint32_t parent;
    char name[LUFIRAFS_MAX_NAME + 1];
    split_path(dest_path, &parent, &name[0]);

    uint32_t file_ino;
    if (lookup_in_dir(parent, name, &file_ino) != 0) {
        file_ino = alloc_inode();
        lufirafs_inode_t inode;
        memset(&inode, 0, sizeof(inode));
        inode.mode = LUFIRAFS_MODE_FILE;
        inode.links_count = 1;
        write_inode(file_ino, &inode);
        add_dirent(parent, name, file_ino);
    }

    lufirafs_inode_t inode;
    read_inode(file_ino, &inode);

    uint32_t remaining = (uint32_t)fsize;
    uint32_t pos = 0;
    while (remaining > 0) {
        uint32_t block_index = pos / LUFIRAFS_BLOCK_SIZE;
        uint32_t block_off = pos % LUFIRAFS_BLOCK_SIZE;
        uint32_t chunk = LUFIRAFS_BLOCK_SIZE - block_off;
        if (chunk > remaining) chunk = remaining;

        uint32_t bn = inode_get_block(&inode, block_index, 1);
        memcpy(block_ptr(bn) + block_off, data + pos, chunk);

        pos += chunk;
        remaining -= chunk;
    }
    inode.size = (uint32_t)fsize;
    write_inode(file_ino, &inode);
    write_superblock();

    free(data);
    printf("mkfs_lufirafs: put %s -> %s (%ld bytes)\n", host_file, dest_path, fsize);
}

static void load_region(const char *image_path, long offset, uint32_t size) {
    FILE *f = fopen(image_path, "r+b");
    if (!f) { fprintf(stderr, "mkfs_lufirafs: cannot open image %s\n", image_path); exit(1); }

    fseek(f, 0, SEEK_END);
    long image_size = ftell(f);
    if (offset + (long)size > image_size) {
        fprintf(stderr, "mkfs_lufirafs: region [%ld..%ld) exceeds image size %ld\n",
                offset, offset + (long)size, image_size);
        fclose(f);
        exit(1);
    }

    g_image = (uint8_t*)malloc(size);
    if (!g_image) { fprintf(stderr, "mkfs_lufirafs: out of memory\n"); exit(1); }
    g_size = size;

    fseek(f, offset, SEEK_SET);
    if (fread(g_image, 1, size, f) != size) {
        fprintf(stderr, "mkfs_lufirafs: short read of region\n");
        exit(1);
    }
    fclose(f);

    memcpy(&g_sb, g_image, sizeof(g_sb));
}

static void save_region(const char *image_path, long offset) {
    FILE *f = fopen(image_path, "r+b");
    if (!f) { fprintf(stderr, "mkfs_lufirafs: cannot reopen image %s\n", image_path); exit(1); }
    fseek(f, offset, SEEK_SET);
    if (fwrite(g_image, 1, g_size, f) != g_size) {
        fprintf(stderr, "mkfs_lufirafs: short write of region\n");
        exit(1);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr,
            "Usage:\n"
            "  %s format <image> <offset> <size>\n"
            "  %s mkdir  <image> <offset> <size> </dest/path>\n"
            "  %s put    <image> <offset> <size> <host_file> </dest/path>\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }

    const char *cmd = argv[1];
    const char *image_path = argv[2];
    long offset = strtol(argv[3], NULL, 0);
    uint32_t size = (uint32_t)strtoul(argv[4], NULL, 0);

    if (strcmp(cmd, "format") == 0) {
        g_image = (uint8_t*)malloc(size);
        if (!g_image) { fprintf(stderr, "mkfs_lufirafs: out of memory\n"); return 1; }
        g_size = size;
        cmd_format();
        save_region(image_path, offset);
    } else if (strcmp(cmd, "mkdir") == 0) {
        if (argc < 6) { fprintf(stderr, "mkfs_lufirafs: mkdir needs a path\n"); return 1; }
        load_region(image_path, offset, size);
        cmd_mkdir(argv[5]);
        write_superblock();
        save_region(image_path, offset);
    } else if (strcmp(cmd, "put") == 0) {
        if (argc < 7) { fprintf(stderr, "mkfs_lufirafs: put needs <host_file> <dest_path>\n"); return 1; }
        load_region(image_path, offset, size);
        cmd_put(argv[5], argv[6]);
        save_region(image_path, offset);
    } else {
        fprintf(stderr, "mkfs_lufirafs: unknown command '%s'\n", cmd);
        return 1;
    }

    free(g_image);
    return 0;
}
