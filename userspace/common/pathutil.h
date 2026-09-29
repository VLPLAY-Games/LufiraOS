// pathutil.h — общий для нескольких userspace-программ (v0.7 план, этап 5,
// под-этап 1: cp/mv/ls) хелпер, которого нет в libc/.
//
// SYS_OPEN резолвит путь ВСЕГДА от корня (см. комментарий у sys_open() в
// kernel/system/syscall/syscall.c) — в отличие от SYS_MKDIR/UNLINK/RMDIR/
// CHDIR/CHMOD/CHOWN, которые уже резолвят от cwd_inode процесса. Любая
// программа, открывающая файл по пути, который МОЖЕТ быть относительным
// (аргумент от пользователя, а не всегда абсолютный литерал), должна сама
// склеить его с SYS_GETCWD перед SYS_OPEN — иначе относительный путь
// неожиданно резолвится от корня, а не от текущего каталога процесса.
#pragma once

#include <lufira/syscall.h>

// path == "." — частый случай (ls/rm без аргумента) — отдельная ветка,
// чтобы не открывать "<cwd>/." (тоже сработало бы, "." — настоящая запись
// в каждом каталоге LufiraFS, но короче и яснее взять cwd как есть).
static inline int resolve_path(const char *path, char *out, int out_size) {
    if (path[0] == '/') {
        int i = 0;
        while (path[i] && i < out_size - 1) { out[i] = path[i]; i++; }
        out[i] = '\0';
        return 0;
    }

    char cwd[256];
    if (sys_getcwd(cwd, sizeof(cwd)) < 0) return -1;

    if (path[0] == '.' && path[1] == '\0') {
        int i = 0;
        while (cwd[i] && i < out_size - 1) { out[i] = cwd[i]; i++; }
        out[i] = '\0';
        return 0;
    }

    int pos = 0;
    while (cwd[pos] && pos < out_size - 2) { out[pos] = cwd[pos]; pos++; }
    if (pos > 0 && out[pos - 1] != '/') out[pos++] = '/';
    int i = 0;
    while (path[i] && pos < out_size - 1) out[pos++] = path[i++];
    out[pos] = '\0';
    return 0;
}
