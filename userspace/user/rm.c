// rm.c — вынос "rm"/"rm *" (kernel/shell/commands/filesystem.c,
// command_rm()/command_rm_all()) в отдельную userspace-программу (v0.7
// план, этап 5, под-этап 1).
//
// Одиночное имя — прямой перенос: SYS_UNLINK, как и SYS_MKDIR, уже
// резолвит от cwd_inode процесса, склеивать путь через SYS_GETCWD не
// нужно. "rm *" — единственный случай, где нужно ОТКРЫТЬ саму текущую
// директорию для перечисления (см. userspace/common/pathutil.h: SYS_OPEN,
// в отличие от SYS_UNLINK, резолвит только от корня, так что "." без
// склейки с cwd открыл бы корень, а не текущий каталог).
//
// Список имён на удаление — через malloc(), не локальный массив на стеке:
// тот же ~15KB (256 записей * 60 байт), что уже triple-fault'ил
// kernel-native command_rm_all() на 16KB ring0-стеке (см. комментарий там)
// — у userspace-процесса стек тоже 16KB (USER_STACK_SIZE, process.h),
// так что риск ровно тот же, если бы этот массив остался на стеке.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lufira/syscall.h>
#include "../common/pathutil.h"

#define MAX_NAMES 256
#define NAME_CAP  60

static void remove_one(const char *name, int *removed, int *errors) {
    long r = sys_unlink(name);
    if (r == 0) {
        printf("  Removed: %s\n", name);
        (*removed)++;
    } else {
        if (r == -ENOENT) printf("  Failed to remove: %s (not found)\n", name);
        else if (r == -EACCES) printf("  Failed to remove: %s (permission denied)\n", name);
        else printf("  Failed to remove: %s\n", name);
        (*errors)++;
    }
}

static int remove_all(void) {
    char cwd_abs[256];
    resolve_path(".", cwd_abs, sizeof(cwd_abs));

    long fd = sys_open(cwd_abs, O_RDONLY, 0);
    if (fd < 0) {
        printf("rm: cannot open directory\n");
        return 1;
    }

    char (*names)[NAME_CAP] = (char (*)[NAME_CAP])malloc(MAX_NAMES * NAME_CAP);
    if (!names) {
        printf("rm: not enough memory\n");
        sys_close((int)fd);
        return 1;
    }

    int count = 0;
    struct lufira_dirent ent;
    while (sys_readdir((int)fd, &ent) > 0 && count < MAX_NAMES) {
        if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0) continue;
        strncpy(names[count], ent.name, NAME_CAP - 1);
        names[count][NAME_CAP - 1] = '\0';
        count++;
    }
    sys_close((int)fd);

    int removed = 0, errors = 0;
    for (int i = 0; i < count; i++) remove_one(names[i], &removed, &errors);

    printf("Removed %d item(s)", removed);
    if (errors > 0) printf(", %d error(s)", errors);
    printf("\n");

    free(names);
    return errors > 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: rm <name> or rm *\n");
        return 1;
    }

    if (strcmp(argv[1], "*") == 0) return remove_all();

    int removed = 0, errors = 0;
    remove_one(argv[1], &removed, &errors);
    return errors > 0 ? 1 : 0;
}
