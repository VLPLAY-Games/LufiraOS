// cp.c — вынос "cp" (kernel/shell/commands/filesystem.c, command_cp()) в
// отдельную userspace-программу (v0.7 план, этап 5, под-этап 1).
//
// В отличие от kernel-native версии, нет отдельной ручной проверки прав
// (check_perm() там читал lufirafs_inode_t напрямую) — SYS_OPEN уже сам
// возвращает -EACCES, когда доступ запрещён (kernel/system/syscall/
// syscall.c), так что пользовательской программе достаточно разобрать код
// ошибки самого syscall'а, как и делает любая обычная Unix-программа.
//
// SYS_OPEN резолвит путь только от корня — оба пути (source и destination)
// сначала склеиваются с cwd через resolve_path() (userspace/common/
// pathutil.h), иначе "cp a.txt b.txt" сработал бы только если процесс
// в корне.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lufira/syscall.h>
#include "../common/pathutil.h"

static long read_whole(const char *path, char **out_buf, long *out_len) {
    long fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) return fd;

    long size = sys_lseek((int)fd, 0, SEEK_END);
    sys_lseek((int)fd, 0, SEEK_SET);
    if (size < 0) { sys_close((int)fd); return -1; }

    char *buf = (char *)malloc(size > 0 ? (size_t)size : 1);
    if (!buf) { sys_close((int)fd); return -1; }

    long total = 0;
    while (total < size) {
        long n = sys_read((int)fd, buf + total, (unsigned long)(size - total));
        if (n <= 0) break;
        total += n;
    }
    sys_close((int)fd);

    *out_buf = buf;
    *out_len = total;
    return 0;
}

static long write_whole(const char *path, const void *data, long len) {
    long fd = sys_open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return fd;

    long pos = 0;
    const char *p = (const char *)data;
    while (pos < len) {
        long n = sys_write((int)fd, p + pos, (unsigned long)(len - pos));
        if (n <= 0) { sys_close((int)fd); return -1; }
        pos += n;
    }
    sys_close((int)fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: cp <source> <destination>\n");
        return 1;
    }
    const char *src = argv[1];
    const char *dst = argv[2];

    char src_abs[256], dst_abs[256];
    resolve_path(src, src_abs, sizeof(src_abs));
    resolve_path(dst, dst_abs, sizeof(dst_abs));

    if (strcmp(src_abs, dst_abs) == 0) {
        printf("cp: cannot copy '%s' to itself\n", src);
        return 1;
    }

    char *buf;
    long len;
    long rc = read_whole(src_abs, &buf, &len);
    if (rc < 0) {
        if (rc == -ENOENT) printf("cp: source file not found: %s\n", src);
        else if (rc == -EACCES) printf("cp: permission denied: %s\n", src);
        else printf("cp: error opening source file\n");
        return 1;
    }

    rc = write_whole(dst_abs, buf, len);
    free(buf);
    if (rc < 0) {
        if (rc == -EACCES) printf("cp: permission denied: %s\n", dst);
        else printf("cp: error creating destination file\n");
        return 1;
    }

    printf("Copied '%s' to '%s' (%ld bytes)\n", src, dst, len);
    return 0;
}
