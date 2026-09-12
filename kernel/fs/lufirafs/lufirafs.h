#pragma once

#include "lufirafs_format.h"
#include "fs/vfs/vfs.h"

// Рабочее (in-memory, ядерное) состояние одной смонтированной LufiraFS.
// Как и fat_fs_t — единственный на всю систему экземпляр (extern lufirafs_t
// lufirafs; в kernel.c), весь диск целиком отражён в память при загрузке
// (bi->FATImageBase), запись идёт в эту копию, а на реальный диск
// сбрасывается через dirty-битовую карту (см. lufirafs_sync()).
typedef struct {
    uint8_t *image;          // указатель на начало РЕГИОНА этой ФС в RAM
    uint32_t image_size;     // байт в регионе
    uint32_t lba_offset;     // смещение региона в АБСОЛЮТНЫХ LBA реального
                              // диска — нужно, чтобы lufirafs_sync() писал
                              // грязные блоки по правильным адресам через
                              // disk_write_sectors() (см. kernel/drivers/disk)
    lufirafs_superblock_t sb;
    uint8_t *dirty_bitmap;    // 1 бит на блок ВСЕГО региона (kmalloc'd)
} lufirafs_t;

int lufirafs_init(lufirafs_t *fs, void *image, uint32_t image_size, uint32_t lba_offset);
extern int lufirafs_mounted; // 1 после успешного lufirafs_init() — для devmode.c/klog.c

// Курсор чтения каталога.
typedef struct {
    lufirafs_t *fs;
    uint32_t dir_ino;
    uint32_t index;
} lufirafs_dir_t;

// ===== Низкоуровневое API (по номеру inode) =====

int lufirafs_read_inode(lufirafs_t *fs, uint32_t ino, lufirafs_inode_t *out);
int lufirafs_write_inode(lufirafs_t *fs, uint32_t ino, const lufirafs_inode_t *in);

// path — абсолютный ("/a/b") или относительный (тогда ищется начиная от
// start_inode, обычно это cwd вызывающего). "." и ".." — обычные записи
// каталога (создаются автоматически при mkdir/форматировании), поэтому
// никакой отдельной логики для них не нужно.
int lufirafs_lookup(lufirafs_t *fs, uint32_t start_inode, const char *path, uint32_t *out_inode);

// Разбивает path на (родительский inode, последний компонент имени) — для
// create/mkdir/unlink, которым нужен ещё не обязательно существующий
// целевой компонент.
int lufirafs_resolve_parent(lufirafs_t *fs, uint32_t start_inode, const char *path,
                             uint32_t *out_parent, char *out_name);

// mode — LUFIRAFS_MODE_FILE или LUFIRAFS_MODE_DIR. Для директорий сразу
// создаёт "." и "..". Ошибки: -1 неверные параметры/уже существует,
// -2 нет свободного inode, -3 не удалось добавить запись в каталог.
int lufirafs_create(lufirafs_t *fs, uint32_t parent_ino, const char *name, uint32_t mode, uint32_t *out_ino);

// Удаляет файл ИЛИ ПУСТУЮ директорию. -1 не найден/неверные параметры,
// -2 директория не пуста, -3 попытка удалить корень.
int lufirafs_unlink(lufirafs_t *fs, uint32_t parent_ino, const char *name);

int lufirafs_read(lufirafs_t *fs, uint32_t ino, uint32_t offset, void *buf, uint32_t count);
int lufirafs_write(lufirafs_t *fs, uint32_t ino, uint32_t offset, const void *buf, uint32_t count);
int lufirafs_truncate(lufirafs_t *fs, uint32_t ino, uint32_t new_size);

int lufirafs_opendir(lufirafs_t *fs, uint32_t ino, lufirafs_dir_t *dir);
int lufirafs_readdir(lufirafs_dir_t *dir, lufirafs_dirent_t *out); // 0 = есть запись, -1 = конец

// Реально занятые БЛОКИ диска под inode — для файла это его собственные
// блоки данных (+1 за косвенный, если есть), для директории — рекурсивно
// ещё и всё её содержимое. То же самое, что показывает `du` в настоящих
// Unix (место НА ДИСКЕ, round-up до block_size), а не логический размер
// файла (inode.size).
uint32_t lufirafs_du_blocks(lufirafs_t *fs, uint32_t ino);

// Дописывает все "грязные" блоки на реальный диск через drivers/disk.
// Аналог fat_sync()/fat_flush() — вызывается и после мутирующих операций,
// и явно на reboot/shutdown (см. shell/commands/system.c).
void lufirafs_sync(lufirafs_t *fs);
void lufirafs_flush(lufirafs_t *fs);

// ===== VFS-обвязка (аналог vfs_fat_*, см. fs/fat/fat_vfs.c) =====

int vfs_open_lufirafs(const char *path, int flags);
int vfs_lufirafs_create(const char *path);
int vfs_lufirafs_mkdir(const char *path);
int vfs_lufirafs_unlink(const char *path);
inode_t* vfs_lufirafs_lookup(const char *path);
inode_t* vfs_lufirafs_get_root(void);
