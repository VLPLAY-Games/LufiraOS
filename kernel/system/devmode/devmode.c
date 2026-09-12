#include "devmode.h"
#include "fs/lufirafs/lufirafs.h"
#include "lib/string.h"

#define DEVMODE_FLAG_PATH "/system/devmode.flag"

extern lufirafs_t lufirafs;
extern int lufirafs_mounted;

static int g_devmode_enabled = 0;

// Читает inode по номеру напрямую из образа — без lufirafs_t/kmalloc, только
// память + суперблок. Нужно для devmode_probe_early(), которая вызывается ДО
// heap_init() (полноценный lufirafs_init() требует кучу под dirty_bitmap).
static void raw_read_inode(const uint8_t *image, const lufirafs_superblock_t *sb,
                            uint32_t ino, lufirafs_inode_t *out) {
    uint32_t byte_off = sb->inode_table_start * LUFIRAFS_BLOCK_SIZE
                       + (ino - 1) * LUFIRAFS_INODE_SIZE;
    memcpy(out, image + byte_off, sizeof(*out));
}

// Ищет name среди прямых блоков директории dir_ino (без косвенного блока —
// /system и /logs всегда маленькие, в один блок).
static int raw_lookup_in_dir(const uint8_t *image, const lufirafs_superblock_t *sb,
                              uint32_t dir_ino, const char *name, uint32_t *out_ino) {
    lufirafs_inode_t dir_inode;
    raw_read_inode(image, sb, dir_ino, &dir_inode);
    if (dir_inode.mode != LUFIRAFS_MODE_DIR) return -1;

    uint32_t nblocks = (dir_inode.size + LUFIRAFS_BLOCK_SIZE - 1) / LUFIRAFS_BLOCK_SIZE;
    if (nblocks > LUFIRAFS_DIRECT_BLOCKS) nblocks = LUFIRAFS_DIRECT_BLOCKS;

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t block_num = dir_inode.direct[b];
        if (!block_num) continue;

        const uint8_t *block = image + (uint64_t)block_num * LUFIRAFS_BLOCK_SIZE;
        for (uint32_t i = 0; i < LUFIRAFS_DIRENTS_PER_BLOCK; i++) {
            const lufirafs_dirent_t *ent = (const lufirafs_dirent_t*)(block + i * LUFIRAFS_DIRENT_SIZE);
            if (ent->inode != 0 && strcmp(ent->name, name) == 0) {
                *out_ino = ent->inode;
                return 0;
            }
        }
    }
    return -1;
}

// Устанавливает начальное значение devmode ДО того, как файловая система
// вообще смонтирована (см. вызов в самом начале _start() в kernel.c) — иначе
// самые первые строки загрузки (GDT/TSS/IDT/PIC/PMM/paging/heap) не могли бы
// узнать о флаге, пока не пройдёт весь пусть до lufirafs_init(). devmode_init()
// ниже (вызывается после настоящего монтирования) — источник истины и
// перезатирает результат этой ранней проверки.
void devmode_probe_early(const void *fs_image, uint32_t fs_size) {
    if (!fs_image || fs_size < LUFIRAFS_BLOCK_SIZE) return;

    const uint8_t *image = (const uint8_t*)fs_image;
    lufirafs_superblock_t sb;
    memcpy(&sb, image, sizeof(sb));
    if (sb.magic != LUFIRAFS_MAGIC || sb.block_size != LUFIRAFS_BLOCK_SIZE) return;

    uint32_t system_ino;
    if (raw_lookup_in_dir(image, &sb, sb.root_inode, "system", &system_ino) != 0) return;

    uint32_t flag_ino;
    g_devmode_enabled = (raw_lookup_in_dir(image, &sb, system_ino, "devmode.flag", &flag_ino) == 0);
}

void devmode_init(void) {
    uint32_t ino;
    g_devmode_enabled = (lufirafs_lookup(&lufirafs, LUFIRAFS_ROOT_INODE, DEVMODE_FLAG_PATH, &ino) == 0);
}

int devmode_is_enabled(void) {
    return g_devmode_enabled;
}

int devmode_set(int enabled) {
    if (!lufirafs_mounted) return -1;

    uint32_t parent;
    char name[LUFIRAFS_MAX_NAME + 1];
    if (lufirafs_resolve_parent(&lufirafs, LUFIRAFS_ROOT_INODE, DEVMODE_FLAG_PATH, &parent, name) != 0)
        return -1;

    uint32_t ino;
    int exists = (lufirafs_lookup(&lufirafs, LUFIRAFS_ROOT_INODE, DEVMODE_FLAG_PATH, &ino) == 0);

    if (enabled && !exists) {
        uint32_t out_ino;
        if (lufirafs_create(&lufirafs, parent, name, LUFIRAFS_MODE_FILE, &out_ino) != 0)
            return -1;
    } else if (!enabled && exists) {
        if (lufirafs_unlink(&lufirafs, parent, name) != 0)
            return -1;
    }

    lufirafs_sync(&lufirafs);
    g_devmode_enabled = enabled ? 1 : 0;
    return 0;
}
