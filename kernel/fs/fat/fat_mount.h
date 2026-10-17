#pragma once

#include "lib/types.h"

// VFS-интеграция USB/FAT: open/read/write/mkdir/unlink/readdir работают на
// смонтированном пути напрямую через vfs_open()/sys_open() (vfs_fat_*() в
// vfs.c), в дополнение к mountls/mountcat/mountwrite из
// kernel/shell/commands/mount.c.
//
// Не полноценный filesystem_t-реестр и не замена fat_vfs.c: тот держит один
// глобальный fatfs и ожидает образ целиком в fs->image, а mount.c хранит N
// параллельных fat_fs_t в массиве — поэтому здесь свой реестр префиксов
// пути ("/mnt/usb0" -> fat_fs_t).
//
// ГРАНИЦА: только корневой каталог монтирования — fat_open()/fat_read_file()/
// fat_write_file() ищут лишь через find_in_root(), а fat_lookup_path()/
// fat_resolve_parent() объявлены, но не реализованы. Многоуровневый
// резолвер пришлось бы писать поверх непроверенного на реальном диске
// cluster-allocation кода — риск порчи данных на флешке не оправдан.
//
// REAL-TIME ЗАПИСЬ: в отличие от mount.c (синк по unmount), здесь
// sync_mount_to_usb() вызывается сразу после каждого write()/mkdir()/
// create()/unlink() — как lufirafs_file_write() вызывает lufirafs_sync().
int vfs_fat_mount(int usb_index, const char *prefix);
int vfs_fat_unmount(const char *prefix);

struct inode;
struct file_t;

// -2, если path не попадает под текущие монтирования — vfs.c идёт дальше к LufiraFS.
int vfs_fat_open(const char *path, int flags);
int vfs_fat_mkdir(const char *path);
int vfs_fat_unlink(const char *path);
struct inode *vfs_fat_lookup(const char *path);

// Для `mount`/`unmount` без аргументов (список активных) — SYS_MOUNT/SYS_UNMOUNT.
int vfs_fat_mount_count(void);
