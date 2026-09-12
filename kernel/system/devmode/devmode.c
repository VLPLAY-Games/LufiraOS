#include "devmode.h"
#include "fs/lufirafs/lufirafs.h"
#include "lib/string.h"

#define DEVMODE_FLAG_PATH "/system/devmode.flag"

extern lufirafs_t lufirafs;
extern int lufirafs_mounted;

static int g_devmode_enabled = 0;

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
