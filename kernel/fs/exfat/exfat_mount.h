#pragma once

#include "lib/types.h"

// VFS/USB-интеграция exFAT — та же модель, что ext2_mount.h/fat_mount.h.
#define EXFAT_MOUNT_MAX        2
#define EXFAT_MOUNT_PREFIX_MAX 32

struct inode;

int vfs_exfat_mount(int usb_index, const char *prefix);
int vfs_exfat_unmount(const char *prefix);
int vfs_exfat_mount_count(void);

int vfs_exfat_open(const char *path, int flags);
int vfs_exfat_mkdir(const char *path);
int vfs_exfat_unlink(const char *path);
struct inode *vfs_exfat_lookup(const char *path);
