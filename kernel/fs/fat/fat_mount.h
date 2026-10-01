#pragma once

#include "lib/types.h"

// Настоящая VFS-интеграция USB/FAT-монтирования — v0.7 план, этап 5,
// под-этап 6 (продолжение после userspace-шелла): обычные open/read/
// write/mkdir/unlink/readdir работают на смонтированном пути напрямую
// через vfs_open()/sys_open() и т.п. (см. вызовы vfs_fat_*() в vfs.c),
// БЕЗ отдельных mountls/mountcat/mountwrite команд (те остаются в
// kernel/shell/commands/mount.c как раньше — мёртвый кернел-native код,
// не трогается).
//
// Сознательно НЕ полноценный filesystem_t-реестр и НЕ ревайвл fat_vfs.c
// (см. разбор в коммит-сообщении/отчёте — fat_vfs.c держит ОДИН глобальный
// fatfs и ожидает, что весь образ лежит в fs->image непрерывно; текущая
// модель mount.c — N параллельных fat_fs_t в массиве, фундаментально другая
// форма). Вместо этого — маленький свой реестр префиксов пути ("/mnt/usb0"
// -> конкретный fat_fs_t) прямо здесь.
//
// ГРАНИЦА (сознательно, не бага): только КОРНЕВОЙ каталог каждого
// монтирования — fat_open()/fat_read_file()/fat_write_file() в fat.c сами
// по себе root-only (ищут только через find_in_root()), а fat_lookup_path()/
// fat_resolve_parent() из fat.h объявлены, но НИКОГДА не были реализованы
// (grep по fat.c подтверждает) — многоуровневый путь внутри флешки
// потребовал бы писать резолвер с нуля поверх cluster-allocation кода, которого никто
// не проверял на реальном диске; слишком большой риск тихой порчи
// пользовательских данных на флешке для одного прохода. mount.c сам
// признаёт этот же предел в своём комментарии ("ограниченные КОРНЕВЫМ...
// отдельная, более крупная задача") — это не новое ограничение, просто
// теперь обычные cat/cp/ls/rm видят его напрямую, без mountls/mountcat.
//
// REAL-TIME ЗАПИСЬ: в отличие от mount.c (синк на USB только по unmount),
// здесь sync_mount_to_usb() вызывается СРАЗУ после каждого успешного
// write()/mkdir()/create()/unlink() — тот же принцип, что у
// lufirafs_file_write() (lufirafs_vfs.c), вызывающего lufirafs_sync() на
// каждую запись.
int vfs_fat_mount(int usb_index, const char *prefix);
int vfs_fat_unmount(const char *prefix);

struct inode;
struct file_t;

// Возвращают -2, если path не попадает под ни одно текущее монтирование —
// вызывающий (vfs.c) в этом случае просто продолжает как раньше (LufiraFS).
int vfs_fat_open(const char *path, int flags);
int vfs_fat_mkdir(const char *path);
int vfs_fat_unlink(const char *path);
struct inode *vfs_fat_lookup(const char *path);

// Для `mount`/`unmount` без аргументов (список активных) — нужно
// userspace-команде (см. SYS_MOUNT/SYS_UNMOUNT, syscall.c).
int vfs_fat_mount_count(void);
