// du.c — вынос команды "du" (kernel/shell/commands/filesystem.c,
// command_du()) в отдельную userspace-программу. В отличие от df/free/
// cpuload, НЕ требует нового syscall'а вообще: размер файла — через уже
// существующие OPEN+SEEK(SEEK_END)+CLOSE, рекурсия по каталогам — через
// уже существующий READDIR. SYS_STATFS (этап 1, для df) переиспользуется
// только за block_size, чтобы считать "K" так же, как kernel-native
// команда (округление размера вверх до границы блока).
//
// Не побайтовая копия kernel-native вывода: та печатает только
// НЕПОСРЕДСТВЕННЫХ детей плюс общий итог (используя lufirafs_du_blocks() —
// кернел-внутреннюю рекурсивную функцию, недоступную отсюда); эта версия
// делает рекурсию сама и печатает КАЖДЫЙ вложенный файл/каталог на своём
// уровне — тот же смысл команды, честно другая (более многословная)
// реализация с нуля поверх обычных syscalls, что и есть весь смысл этого
// этапа.
//
// Сборка — как у free.c/hello.c.

#include <stdio.h>
#include <lufira/syscall.h>
#include <string.h>

static uint32_t g_block_size = 4096;

static long file_blocks(const char *path) {
    long fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    long size = sys_lseek((int)fd, 0, SEEK_END);
    sys_close((int)fd);
    if (size < 0) return -1;
    return (size + (long)g_block_size - 1) / (long)g_block_size;
}

static void join_path(char *out, int out_size, const char *base, const char *name) {
    int pos = 0;
    while (base[pos] && pos < out_size - 2) { out[pos] = base[pos]; pos++; }
    if (pos > 0 && out[pos - 1] != '/') out[pos++] = '/';
    int i = 0;
    while (name[i] && pos < out_size - 1) out[pos++] = name[i++];
    out[pos] = '\0';
}

// Возвращает суммарные блоки под всем поддеревом path (каталог) — печатает
// по дороге каждую вложенную запись со своим собственным размером, как
// делает "du" в большинстве Unix-подобных систем без флага -s.
static long dir_total_blocks(const char *path) {
    long fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    long total = 0;
    struct lufira_dirent ent;
    for (;;) {
        long n = sys_readdir((int)fd, &ent);
        if (n <= 0) break;
        if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0) continue;

        char child[256];
        join_path(child, (int)sizeof(child), path, ent.name);

        long blocks = (ent.type == LUFIRA_FT_DIRECTORY) ? dir_total_blocks(child) : file_blocks(child);
        if (blocks < 0) blocks = 0;

        unsigned long kb = ((unsigned long)blocks * g_block_size + 1023) / 1024;
        printf("%lu K\t%s\n", kb, child);

        total += blocks;
    }
    sys_close((int)fd);
    return total;
}

int main(int argc, char **argv) {
    const char *target = (argc >= 2) ? argv[1] : ".";

    struct lufira_statfs sfs;
    if (sys_statfs(&sfs) == 0 && sfs.block_size > 0) g_block_size = sfs.block_size;

    long fd = sys_open(target, O_RDONLY, 0);
    if (fd < 0) {
        printf("du: '%s' not found\n", target);
        return 1;
    }
    // "." — настоящая запись в каждом каталоге LufiraFS, так что если это
    // директория, sys_readdir() на свежем fd успеет вернуть хотя бы её —
    // если path оказался обычным файлом, vfs_readdir() откажет сразу
    // (проверка inode->type в vfs.c), это и отличает файл от каталога без
    // отдельного stat()-примитива, которого в этом ядре нет.
    struct lufira_dirent probe;
    int is_dir = sys_readdir((int)fd, &probe) >= 0;
    sys_close((int)fd);

    long total = is_dir ? dir_total_blocks(target) : file_blocks(target);
    if (total < 0) total = 0;

    unsigned long kb = ((unsigned long)total * g_block_size + 1023) / 1024;
    printf("%lu K\ttotal (%s)\n", kb, target);
    return 0;
}
