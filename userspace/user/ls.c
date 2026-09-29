// ls.c — вынос "ls" (kernel/shell/commands/filesystem.c, command_ls()) в
// отдельную userspace-программу (v0.7 план, этап 5, под-этап 1).
//
// Сознательно НЕ порт 1:1: kernel-native версия поддерживает "-l" (права/
// владелец/размер прямо из lufirafs_inode_t) и раскрашивает вывод (прямые
// вызовы console.c). Ни то, ни другое не имеет syscall-эквивалента —
// у ABI этого ядра нет stat()-примитива (см. план фундамента v0.7) и нет
// syscall'а на цвет консоли (курсор/цвет — сознательно отложены на этап 5,
// под-этап 6, где заводится вся консольная подсистема шелла разом). Тот же
// осознанный компромисс, что уже у cpuload.c (потерял per-process
// разбивку) и du.c (не побайтовый повтор форматирования) в этапе 1: список
// имён без цвета и без "-l" — то, что реально можно сделать поверх
// сегодняшних syscalls.
//
// SYS_OPEN резолвит путь только от корня (userspace/common/pathutil.h) —
// без этого "ls" внутри /etc среагировал бы на голое "ls" листингом корня,
// а не /etc.

#include <stddef.h>
#include <stdio.h>
#include <lufira/syscall.h>
#include "../common/pathutil.h"

int main(int argc, char **argv) {
    const char *path = (argc >= 2) ? argv[1] : NULL;

    char abs_path[256];
    if (resolve_path(path ? path : ".", abs_path, sizeof(abs_path)) < 0) {
        printf("ls: cannot resolve current directory\n");
        return 1;
    }

    long fd = sys_open(abs_path, O_RDONLY, 0);
    if (fd < 0) {
        printf("ls: cannot access '%s': No such file or directory\n", path ? path : ".");
        return 1;
    }

    // "." — настоящая запись в каждом каталоге LufiraFS, так что успешный
    // первый sys_readdir() отличает каталог от обычного файла (тот же приём,
    // что уже у du.c) без отдельного stat()-примитива, которого нет.
    struct lufira_dirent probe;
    int is_dir = sys_readdir((int)fd, &probe) > 0;
    sys_close((int)fd);

    if (!is_dir) {
        printf("%s\n", path ? path : ".");
        return 0;
    }

    fd = sys_open(abs_path, O_RDONLY, 0);
    if (fd < 0) {
        printf("ls: cannot access '%s': No such file or directory\n", path ? path : ".");
        return 1;
    }

    int count = 0;
    struct lufira_dirent ent;
    while (sys_readdir((int)fd, &ent) > 0) {
        printf("%s  ", ent.name);
        if (++count % 4 == 0) printf("\n");
    }
    sys_close((int)fd);
    if (count % 4 != 0) printf("\n");
    return 0;
}
