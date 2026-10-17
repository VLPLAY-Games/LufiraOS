#pragma once

#include "lib/types.h"

// RAMFS: чисто оперативная ФС, монтируется по префиксу пути (как
// vfs_fat_mount(), см. fat_mount.h), но без ограничения FAT на корневой
// уровень — обычное дерево узлов с родителем/детьми, растущее по
// требованию через kmalloc()/kfree(), с вложенными каталогами любой
// глубины.
//
// Данные не переживают перезагрузку и никуда не синкаются — ближе к
// tmpfs, чем к постоянному хранилищу. Подходит для /tmp и как песочница.

#define RAMFS_MAX_MOUNTS      4
#define RAMFS_MOUNT_PREFIX_MAX 32
#define RAMFS_MAX_NAME        64

struct inode;

// Монтирует новый пустой инстанс RAMFS по абсолютному префиксу ("/tmp").
// 0 при успехе: -1 плохой prefix, -2 уже занято, -3 нет слотов, -4 kmalloc.
int ramfs_mount(const char *prefix);

// Размонтирует и освобождает всё дерево узлов (данные теряются).
// 0 при успехе, -1 если такого монтирования нет.
int ramfs_unmount(const char *prefix);

int vfs_ramfs_mount_count(void);

// -2, если path не под этим монтированием — vfs.c идёт дальше к LufiraFS.
int vfs_ramfs_open(const char *path, int flags);
int vfs_ramfs_mkdir(const char *path);
int vfs_ramfs_unlink(const char *path);
struct inode *vfs_ramfs_lookup(const char *path);
