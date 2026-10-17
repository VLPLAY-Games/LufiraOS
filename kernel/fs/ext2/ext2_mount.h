#pragma once

#include "lib/types.h"

// VFS/USB-интеграция ext2: реестр префиксов пути -> смонтированный образ,
// как в fat_mount.h, но без ограничения FAT на корневой уровень — ext2.c
// работает с деревом инодов, вложенные каталоги поддержаны нативно.
#define EXT2_MOUNT_MAX        2
#define EXT2_MOUNT_PREFIX_MAX 32

struct inode;

int vfs_ext2_mount(int usb_index, const char *prefix);
int vfs_ext2_unmount(const char *prefix);
int vfs_ext2_mount_count(void);

int vfs_ext2_open(const char *path, int flags);
int vfs_ext2_mkdir(const char *path);
int vfs_ext2_unlink(const char *path);
struct inode *vfs_ext2_lookup(const char *path);
