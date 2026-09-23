#include "../commands.h"
#include "drivers/console/console.h"
#include "../shell.h"
#include "fs/lufirafs/lufirafs.h"
#include "system/elf/elf.h"
#include "system/process/process.h"
#include "system/syscall/syscall.h"
#include "system/mm/heap.h"
#include "system/devmode/devmode.h"
#include "system/klog/klog.h"
#include "system/users/users.h"
#include "lib/string.h"

#define LS_COLOR_DIR  COLOR_LIGHT_BLUE
#define LS_COLOR_FILE COLOR_WHITE
#define LS_COLOR_EXEC COLOR_LIGHT_GREEN

extern lufirafs_t lufirafs;
// cwd_path/cwd_inode теперь макросы поверх current_process->... (shell.h),
// а не отдельные extern-переменные.

// Общая проверка прав для команд шелла (эти команды бьют в lufirafs_*
// напрямую, минуя VFS/syscall.c — см. комментарий в lufirafs_vfs.c). При
// отказе сама печатает "<cmd>: permission denied: <name>".
static int check_perm(uint32_t ino, int want_read, int want_write, int want_exec,
                       const char *cmd, const char *name) {
    lufirafs_inode_t inode;
    if (lufirafs_read_inode(&lufirafs, ino, &inode) != 0) return 0;
    if (!lufirafs_check_access(&inode, current_process->uid, current_process->gid,
                                want_read, want_write, want_exec)) {
        printf("\n%s: permission denied: %s\n", cmd, name);
        return 0;
    }
    return 1;
}

// Все имена в LufiraFS уже приводятся к нижнему регистру ещё на входе в
// шелл (execute_command() лоуеркейсит всю строку до разбора команды),
// поэтому сравнивать суффикс регистронезависимо не нужно.
static int is_executable_name(const char *name) {
    int len = 0;
    while (name[len]) len++;
    return len > 4 && strcmp(name + len - 4, ".elf") == 0;
}

// ls, cd, pwd, mkdir, rm, touch, cat
void command_ls(const char* flags) {
    int long_fmt = 0;
    char path_buf[LUFIRAFS_MAX_NAME * 4];
    const char *path = NULL;

    const char *p = flags ? skip_spaces(flags) : "";
    while (*p) {
        int len = token_length(p);
        if (token_equals(p, "-l")) {
            long_fmt = 1;
        } else {
            int copy_len = len < (int)sizeof(path_buf) - 1 ? len : (int)sizeof(path_buf) - 1;
            for (int i = 0; i < copy_len; i++) path_buf[i] = p[i];
            path_buf[copy_len] = '\0';
            path = path_buf;
        }
        p = skip_spaces(p + len);
    }

    uint32_t target_inode = cwd_inode;
    if (path) {
        uint32_t found;
        if (lufirafs_lookup(&lufirafs, cwd_inode, path, &found) != 0) {
            printf("\nls: cannot access '%s': No such file or directory\n", path);
            return;
        }
        lufirafs_inode_t target;
        lufirafs_read_inode(&lufirafs, found, &target);
        if (target.mode != LUFIRAFS_MODE_DIR) {
            printf("\n%s\n", path);
            return;
        }
        target_inode = found;
    }

    lufirafs_dir_t dir;
    if (lufirafs_opendir(&lufirafs, target_inode, &dir) != 0) {
        printf("\nCannot open directory\n");
        return;
    }

    // Раньше в конце цвет жёстко сбрасывался на LOG_COLOR_INFO, затирая то,
    // что пользователь выставил командой fg. Запоминаем реальный текущий
    // цвет и возвращаемся именно к нему.
    ConsoleColor saved_fg = current_colors.fg_index;

    printf("\n");
    lufirafs_dirent_t entry;
    int count = 0;
    while (lufirafs_readdir(&dir, &entry) == 0) {
        lufirafs_inode_t inode;
        lufirafs_read_inode(&lufirafs, entry.inode, &inode);
        int is_dir = (inode.mode == LUFIRAFS_MODE_DIR);
        int is_exec = !is_dir && is_executable_name(entry.name);

        set_foreground_color(is_dir ? LS_COLOR_DIR : (is_exec ? LS_COLOR_EXEC : LS_COLOR_FILE));

        if (long_fmt) {
            char perm_str[11];
            perm_str[0] = is_dir ? 'd' : '-';
            perm_str[1] = (inode.perm & 0400) ? 'r' : '-';
            perm_str[2] = (inode.perm & 0200) ? 'w' : '-';
            perm_str[3] = (inode.perm & 0100) ? 'x' : '-';
            perm_str[4] = (inode.perm & 0040) ? 'r' : '-';
            perm_str[5] = (inode.perm & 0020) ? 'w' : '-';
            perm_str[6] = (inode.perm & 0010) ? 'x' : '-';
            perm_str[7] = (inode.perm & 0004) ? 'r' : '-';
            perm_str[8] = (inode.perm & 0002) ? 'w' : '-';
            perm_str[9] = (inode.perm & 0001) ? 'x' : '-';
            perm_str[10] = '\0';

            user_entry_t u;
            group_entry_t g;
            int have_user = (users_lookup_by_uid(inode.uid, &u) == 0);
            int have_group = (groups_lookup_by_gid(inode.gid, &g) == 0);

            printf("%s ", perm_str);
            if (have_user) printf("%s ", u.username); else printf("%u ", inode.uid);
            if (have_group) printf("%s ", g.groupname); else printf("%u ", inode.gid);
            printf("%u ", inode.size);
            printf("%s\n", entry.name);
        } else {
            printf("%s  ", entry.name);
            if (++count % 4 == 0) printf("\n");
        }
    }
    set_foreground_color(saved_fg);
    if (!long_fmt && count % 4 != 0) printf("\n");
}

void command_cd(const char* path) {
    if (!path || !*path) return;

    uint32_t new_inode;
    if (lufirafs_lookup(&lufirafs, cwd_inode, path, &new_inode) != 0) {
        printf("\ncd: no such directory: %s\n", path);
        return;
    }

    lufirafs_inode_t inode;
    if (lufirafs_read_inode(&lufirafs, new_inode, &inode) != 0 || inode.mode != LUFIRAFS_MODE_DIR) {
        printf("\ncd: not a directory: %s\n", path);
        return;
    }

    // cwd_path раньше достраивался вручную (конкатенацией/обрезкой строки),
    // что ломалось на "..", абсолютных и составных путях. Проще и надёжнее
    // каждый раз пересчитать его с нуля из нового inode, поднимаясь по
    // реальным записям ".." в файловой системе — так же, как это делает
    // lufirafs_lookup для самого перехода.
    if (lufirafs_get_path(&lufirafs, new_inode, cwd_path, sizeof(cwd_path)) != 0) {
        cwd_path[0] = '/';
        cwd_path[1] = '\0';
    }

    cwd_inode = new_inode;
}

void command_mkdir(const char* name) {
    if (!name || !*name) return;
    if (!check_perm(cwd_inode, 0, 1, 1, "mkdir", name)) return;

    uint32_t out_ino;
    int res = lufirafs_create(&lufirafs, cwd_inode, name, LUFIRAFS_MODE_DIR,
                               current_process->uid, current_process->gid,
                               LUFIRAFS_DEFAULT_DIR_PERM, &out_ino);
    switch (res) {
        case 0:
            lufirafs_sync(&lufirafs);
            printf("\nDirectory created: %s\n", name);
            break;
        case -1:
            printf("\nmkdir: '%s' already exists\n", name);
            break;
        case -2:
            printf("\nmkdir: no free inode\n");
            break;
        case -3:
            printf("\nmkdir: no free space\n");
            break;
        default:
            printf("\nmkdir: failed (error %d)\n", res);
            break;
    }
}

void command_rm(const char* name) {
    if (!name || !*name) {
        printf("\nUsage: rm <name> or rm *\n");
        return;
    }
    // Прав на конкретный удаляемый файл не проверяем (нет sticky bit) —
    // классическое до-sticky-bit поведение Unix: достаточно прав на запись
    // в родительский каталог.
    if (!check_perm(cwd_inode, 0, 1, 1, "rm", name)) return;

    if (strcmp(name, "*") == 0) {
        lufirafs_dir_t dir;
        if (lufirafs_opendir(&lufirafs, cwd_inode, &dir) != 0) {
            printf("\nCannot open directory\n");
            return;
        }

        printf("\n");
        lufirafs_dirent_t entry;
        char names_to_delete[256][LUFIRAFS_MAX_NAME + 1];
        int name_count = 0;

        while (lufirafs_readdir(&dir, &entry) == 0) {
            if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0)
                continue;
            if (name_count >= 256) break;
            strcpy(names_to_delete[name_count], entry.name);
            name_count++;
        }

        int removed_count = 0, error_count = 0;
        for (int i = 0; i < name_count; i++) {
            int res = lufirafs_unlink(&lufirafs, cwd_inode, names_to_delete[i]);
            switch (res) {
                case 0:
                    printf("  Removed: %s\n", names_to_delete[i]);
                    removed_count++;
                    break;
                case -2:
                    printf("  Skipped (not empty): %s\n", names_to_delete[i]);
                    error_count++;
                    break;
                default:
                    printf("  Failed to remove: %s (error %d)\n", names_to_delete[i], res);
                    error_count++;
                    break;
            }
        }
        if (removed_count > 0) lufirafs_sync(&lufirafs);

        printf("\nRemoved %d item(s)", removed_count);
        if (error_count > 0) printf(", %d error(s)", error_count);
        printf("\n");
        return;
    }

    int res = lufirafs_unlink(&lufirafs, cwd_inode, name);
    switch (res) {
        case 0:
            lufirafs_sync(&lufirafs);
            printf("\nRemoved: %s\n", name);
            break;
        case -1:
            printf("\nrm: '%s' not found\n", name);
            break;
        case -2:
            printf("\nrm: directory not empty: %s\n", name);
            break;
        case -3:
            printf("\nrm: cannot remove root\n");
            break;
        default:
            printf("\nrm: failed (error %d)\n", res);
            break;
    }
}

void command_touch(const char* name) {
    if (!name || !*name) {
        printf("\nUsage: touch <filename>\n");
        return;
    }
    if (!check_perm(cwd_inode, 0, 1, 1, "touch", name)) return;

    uint32_t out_ino;
    int res = lufirafs_create(&lufirafs, cwd_inode, name, LUFIRAFS_MODE_FILE,
                               current_process->uid, current_process->gid,
                               LUFIRAFS_DEFAULT_FILE_PERM, &out_ino);
    switch (res) {
        case 0:
            lufirafs_sync(&lufirafs);
            printf("\nFile created: %s\n", name);
            break;
        case -1:
            printf("\ntouch: '%s' already exists\n", name);
            break;
        case -2:
            printf("\ntouch: no free inode\n");
            break;
        case -3:
            printf("\ntouch: no free space\n");
            break;
        default:
            printf("\ntouch: failed (error %d)\n", res);
            break;
    }
}

void command_cat(const char* filename) {
    if (!filename || *filename == '\0') {
        printf("\nUsage: cat <filename>\n");
        return;
    }

    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, cwd_inode, filename, &ino) != 0) {
        printf("\nFile not found: %s\n", filename);
        return;
    }
    if (!check_perm(ino, 1, 0, 0, "cat", filename)) return;

    lufirafs_inode_t inode;
    lufirafs_read_inode(&lufirafs, ino, &inode);

    uint8_t *buf = (uint8_t*)kmalloc(inode.size > 0 ? inode.size : 1);
    if (!buf) {
        printf("\ncat: not enough memory\n");
        return;
    }

    int br = lufirafs_read(&lufirafs, ino, 0, buf, inode.size);
    if (br >= 0) {
        printf("\n--- %s (%u bytes) ---\n", filename, inode.size);
        for (int i = 0; i < br; i++) put_char(buf[i]);
        printf("\n--- end ---\n");
    } else {
        printf("\nError reading file.\n");
    }
    kfree(buf);
}

// run - запуск ELF файла
void command_run(const char *filename) {
    if (!filename || *filename == '\0') {
        printf("\nUsage: run <filename>\n");
        printf("Example: run hello.elf\n");
        return;
    }

    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, cwd_inode, filename, &ino) != 0) {
        printf("\nFile not found: %s\n", filename);
        return;
    }
    if (!check_perm(ino, 0, 0, 1, "run", filename)) return;

    lufirafs_inode_t inode;
    lufirafs_read_inode(&lufirafs, ino, &inode);
    uint32_t fsize = inode.size;

    uint8_t *file_buf = (uint8_t *)kmalloc(fsize);
    if (!file_buf) {
        printf("\nNot enough memory to load %s (%u bytes)\n", filename, fsize);
        return;
    }

    int br = lufirafs_read(&lufirafs, ino, 0, file_buf, fsize);
    if (br <= 0) {
        printf("\nError reading file: %s\n", filename);
        kfree(file_buf);
        return;
    }

    DLOG("\nLoading ELF: %s (%u bytes)...\n", filename, fsize);
    klog("[SHELL] run '%s' (%u bytes)", filename, fsize);

    if (elf_exec(file_buf, fsize, filename) == 0) {
        printf("Process started!\n");
    }
}

// exec - заменяет ТЕКУЩИЙ процесс (shell) программой из filename.
// В отличие от run, ничего нового не создаёт: при успехе управление в
// shell уже не вернётся.
void command_exec(const char *filename) {
    if (!filename || *filename == '\0') {
        printf("\nUsage: exec <filename>\n");
        printf("Example: exec hello.elf\n");
        return;
    }

    // do_exec() идёт через VFS, а там пути ВСЕГДА разрешаются от корня (см.
    // комментарий вверху lufirafs_vfs.c) — в отличие от run(), который бьёт
    // напрямую в lufirafs_lookup(cwd_inode, ...) и потому понимает путь
    // относительно текущей папки шелла. Поэтому сами резолвим filename через
    // cwd_inode и собираем готовый абсолютный путь для do_exec().
    //
    // filename указывает на ФАЙЛ, а не на директорию, поэтому у него самого
    // нет записи ".." (она есть только у директорий) — lufirafs_get_path()
    // нельзя вызвать прямо на нём. Сначала берём родительскую директорию
    // через lufirafs_resolve_parent() (она точно директория, get_path для
    // неё работает), затем приклеиваем к её абсолютному пути имя файла.
    uint32_t parent_ino;
    char leaf[LUFIRAFS_MAX_NAME + 1];
    if (lufirafs_resolve_parent(&lufirafs, cwd_inode, filename, &parent_ino, leaf) != 0) {
        printf("\nExec failed: %s\n", filename);
        return;
    }

    char abs_path[256];
    if (lufirafs_get_path(&lufirafs, parent_ino, abs_path, sizeof(abs_path)) != 0) {
        printf("\nExec failed: %s\n", filename);
        return;
    }

    int pos = 0;
    while (abs_path[pos]) pos++;
    int leaf_len = 0;
    while (leaf[leaf_len]) leaf_len++;
    if (pos + 1 + leaf_len >= (int)sizeof(abs_path)) {
        printf("\nExec failed: %s\n", filename);
        return;
    }
    if (pos == 0 || abs_path[pos - 1] != '/') abs_path[pos++] = '/';
    for (int i = 0; i < leaf_len; i++) abs_path[pos++] = leaf[i];
    abs_path[pos] = '\0';

    if (do_exec(abs_path) != 0) {
        printf("\nExec failed: %s\n", filename);
    }
    // Не освобождаем буфер - он используется процессом
}

// write - запись в файл (перезаписывает целиком)
void command_write(const char *filename) {
    if (!filename || *filename == '\0') {
        printf("\nUsage: write <filename> <text>\n");
        printf("Example: write test.txt Hello World\n");
        return;
    }

    const char *text = filename;
    while (*text && *text != ' ') text++;
    if (*text == ' ') {
        text++;
        while (*text == ' ') text++;
    }

    if (*text == '\0') {
        printf("\nUsage: write <filename> <text>\n");
        return;
    }

    int len = 0;
    while (text[len]) len++;

    uint32_t ino;
    int exists = (lufirafs_lookup(&lufirafs, cwd_inode, filename, &ino) == 0);

    if (exists) {
        if (!check_perm(ino, 0, 1, 0, "write", filename)) return;
    } else {
        if (!check_perm(cwd_inode, 0, 1, 1, "write", filename)) return;
        if (lufirafs_create(&lufirafs, cwd_inode, filename, LUFIRAFS_MODE_FILE,
                             current_process->uid, current_process->gid,
                             LUFIRAFS_DEFAULT_FILE_PERM, &ino) != 0) {
            printf("\nError creating file!\n");
            return;
        }
    }

    lufirafs_truncate(&lufirafs, ino, 0);
    int result = lufirafs_write(&lufirafs, ino, 0, text, len);
    lufirafs_sync(&lufirafs);

    if (result >= 0) {
        printf("\nFile '%s' %s (%d bytes)\n", filename, exists ? "overwritten" : "created and written", len);
    } else {
        printf("Error writing file!\n");
    }
}

// cp - копирование файла
void command_cp(const char *args) {
    if (!args || *args == '\0') {
        printf("\nUsage: cp <source> <destination>\n");
        return;
    }

    const char *src = args;
    while (*src == ' ') src++;

    const char *dst = src;
    while (*dst && *dst != ' ') dst++;
    if (*dst == ' ') {
        dst++;
        while (*dst == ' ') dst++;
    }

    if (*src == '\0' || *dst == '\0') {
        printf("\nUsage: cp <source> <destination>\n");
        return;
    }

    char src_name[256], dst_name[256];
    int i = 0;
    while (src[i] && src[i] != ' ' && i < 255) { src_name[i] = src[i]; i++; }
    src_name[i] = '\0';

    i = 0;
    while (dst[i] && dst[i] != ' ' && i < 255) { dst_name[i] = dst[i]; i++; }
    dst_name[i] = '\0';

    if (strcmp(src_name, dst_name) == 0) {
        printf("\ncp: cannot copy '%s' to itself\n", src_name);
        return;
    }

    uint32_t src_ino;
    if (lufirafs_lookup(&lufirafs, cwd_inode, src_name, &src_ino) != 0) {
        printf("\ncp: source file not found: %s\n", src_name);
        return;
    }
    if (!check_perm(src_ino, 1, 0, 0, "cp", src_name)) return;

    lufirafs_inode_t src_inode;
    lufirafs_read_inode(&lufirafs, src_ino, &src_inode);

    uint8_t *buf = (uint8_t *)kmalloc(src_inode.size > 0 ? src_inode.size : 1);
    if (!buf) {
        printf("\ncp: not enough memory\n");
        return;
    }

    int br = lufirafs_read(&lufirafs, src_ino, 0, buf, src_inode.size);
    if (br < 0) {
        printf("\ncp: error reading source file\n");
        kfree(buf);
        return;
    }

    // dst_name может содержать путь к каталогу (например "system/copy.txt") —
    // lufirafs_create принимает только (родитель, простое имя), поэтому
    // сначала разбиваем dst_name на родительский inode и конечное имя, а не
    // передаём его целиком как имя записи в cwd_inode.
    uint32_t dst_parent;
    char dst_leaf[LUFIRAFS_MAX_NAME + 1];
    if (lufirafs_resolve_parent(&lufirafs, cwd_inode, dst_name, &dst_parent, dst_leaf) != 0) {
        printf("\ncp: invalid destination path: %s\n", dst_name);
        kfree(buf);
        return;
    }

    uint32_t dst_ino;
    if (lufirafs_lookup(&lufirafs, dst_parent, dst_leaf, &dst_ino) != 0) {
        if (!check_perm(dst_parent, 0, 1, 1, "cp", dst_name)) { kfree(buf); return; }
        if (lufirafs_create(&lufirafs, dst_parent, dst_leaf, LUFIRAFS_MODE_FILE,
                             current_process->uid, current_process->gid,
                             LUFIRAFS_DEFAULT_FILE_PERM, &dst_ino) != 0) {
            printf("\ncp: error creating destination file\n");
            kfree(buf);
            return;
        }
    } else {
        if (!check_perm(dst_ino, 0, 1, 0, "cp", dst_name)) { kfree(buf); return; }
        lufirafs_truncate(&lufirafs, dst_ino, 0);
    }

    int result = lufirafs_write(&lufirafs, dst_ino, 0, buf, src_inode.size);
    lufirafs_sync(&lufirafs);

    if (result >= 0) {
        printf("\nCopied '%s' to '%s' (%u bytes)\n", src_name, dst_name, src_inode.size);
    } else {
        printf("\ncp: error writing destination file\n");
    }

    kfree(buf);
}

// mv - перемещение/переименование файла
void command_mv(const char *args) {
    if (!args || *args == '\0') {
        printf("\nUsage: mv <source> <destination>\n");
        return;
    }

    const char *src = args;
    while (*src == ' ') src++;

    const char *dst = src;
    while (*dst && *dst != ' ') dst++;
    if (*dst == ' ') {
        dst++;
        while (*dst == ' ') dst++;
    }

    if (*src == '\0' || *dst == '\0') {
        printf("\nUsage: mv <source> <destination>\n");
        return;
    }

    char src_name[256], dst_name[256];
    int i = 0;
    while (src[i] && src[i] != ' ' && i < 255) { src_name[i] = src[i]; i++; }
    src_name[i] = '\0';

    i = 0;
    while (dst[i] && dst[i] != ' ' && i < 255) { dst_name[i] = dst[i]; i++; }
    dst_name[i] = '\0';

    if (strcmp(src_name, dst_name) == 0) {
        printf("\nmv: cannot move '%s' to itself\n", src_name);
        return;
    }

    // Как и в cp: src_name/dst_name могут содержать путь к каталогу, а не
    // просто имя файла в cwd_inode — резолвим родителя для каждого из них
    // отдельно, чтобы unlink() снял запись из настоящего родительского
    // каталога источника, а create()/lookup() для назначения работали в его
    // собственном каталоге, а не всегда в cwd_inode.
    uint32_t src_parent;
    char src_leaf[LUFIRAFS_MAX_NAME + 1];
    if (lufirafs_resolve_parent(&lufirafs, cwd_inode, src_name, &src_parent, src_leaf) != 0) {
        printf("\nmv: invalid source path: %s\n", src_name);
        return;
    }

    uint32_t src_ino;
    if (lufirafs_lookup(&lufirafs, src_parent, src_leaf, &src_ino) != 0) {
        printf("\nmv: source file not found: %s\n", src_name);
        return;
    }
    // mv читает src целиком и потом unlink'ает его из родителя — нужны и
    // read на сам файл, и write+exec на его родительский каталог.
    if (!check_perm(src_ino, 1, 0, 0, "mv", src_name)) return;
    if (!check_perm(src_parent, 0, 1, 1, "mv", src_name)) return;

    lufirafs_inode_t src_inode;
    lufirafs_read_inode(&lufirafs, src_ino, &src_inode);

    uint8_t *buf = (uint8_t *)kmalloc(src_inode.size > 0 ? src_inode.size : 1);
    if (!buf) {
        printf("\nmv: not enough memory\n");
        return;
    }

    int br = lufirafs_read(&lufirafs, src_ino, 0, buf, src_inode.size);
    if (br < 0) {
        printf("\nmv: error reading source file\n");
        kfree(buf);
        return;
    }

    uint32_t dst_parent;
    char dst_leaf[LUFIRAFS_MAX_NAME + 1];
    if (lufirafs_resolve_parent(&lufirafs, cwd_inode, dst_name, &dst_parent, dst_leaf) != 0) {
        printf("\nmv: invalid destination path: %s\n", dst_name);
        kfree(buf);
        return;
    }

    uint32_t dst_ino;
    if (lufirafs_lookup(&lufirafs, dst_parent, dst_leaf, &dst_ino) != 0) {
        if (!check_perm(dst_parent, 0, 1, 1, "mv", dst_name)) { kfree(buf); return; }
        if (lufirafs_create(&lufirafs, dst_parent, dst_leaf, LUFIRAFS_MODE_FILE,
                             current_process->uid, current_process->gid,
                             LUFIRAFS_DEFAULT_FILE_PERM, &dst_ino) != 0) {
            printf("\nmv: error creating destination file\n");
            kfree(buf);
            return;
        }
    } else {
        if (!check_perm(dst_ino, 0, 1, 0, "mv", dst_name)) { kfree(buf); return; }
        lufirafs_truncate(&lufirafs, dst_ino, 0);
    }
    lufirafs_write(&lufirafs, dst_ino, 0, buf, src_inode.size);

    lufirafs_unlink(&lufirafs, src_parent, src_leaf);
    lufirafs_sync(&lufirafs);

    printf("\nMoved '%s' to '%s' (%u bytes)\n", src_name, dst_name, src_inode.size);

    kfree(buf);
}

// rename - переименование
void command_rename(const char *args) {
    command_mv(args);  // rename = mv
}

// df - свободное/занятое место на диске
void command_df(void) {
    uint32_t block_size = lufirafs.sb.block_size;
    uint32_t total = lufirafs.sb.total_blocks;
    uint32_t free_blocks = lufirafs.sb.free_blocks;
    uint32_t used = total - free_blocks;

    uint64_t total_kb = ((uint64_t)total * block_size) / 1024;
    uint64_t used_kb = ((uint64_t)used * block_size) / 1024;
    uint64_t free_kb = ((uint64_t)free_blocks * block_size) / 1024;

    printf("\nFilesystem: LufiraFS (block size %u bytes)\n", block_size);
    printf("Total: %lu KB\n", total_kb);
    printf("Used:  %lu KB\n", used_kb);
    printf("Free:  %lu KB\n", free_kb);
    printf("Inodes: %u total, %u free\n", lufirafs.sb.inode_count, lufirafs.sb.free_inodes);
}

// du - место на диске, занятое файлом/директорией (рекурсивно)
void command_du(const char *path) {
    const char *target = (path && *path) ? path : ".";

    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, cwd_inode, target, &ino) != 0) {
        printf("\ndu: '%s' not found\n", target);
        return;
    }

    lufirafs_inode_t inode;
    lufirafs_read_inode(&lufirafs, ino, &inode);
    uint32_t block_size = lufirafs.sb.block_size;

    printf("\n");
    if (inode.mode == LUFIRAFS_MODE_DIR) {
        lufirafs_dir_t dir;
        lufirafs_opendir(&lufirafs, ino, &dir);
        lufirafs_dirent_t ent;
        while (lufirafs_readdir(&dir, &ent) == 0) {
            if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0) continue;
            uint32_t blocks = lufirafs_du_blocks(&lufirafs, ent.inode);
            uint64_t kb = ((uint64_t)blocks * block_size + 1023) / 1024;
            printf("%lu K\t%s\n", kb, ent.name);
        }
    }

    uint32_t total_blocks = lufirafs_du_blocks(&lufirafs, ino);
    uint64_t total_kb = ((uint64_t)total_blocks * block_size + 1023) / 1024;
    printf("%lu K\ttotal (%s)\n", total_kb, target);
}

// edit - простой редактор (дописывает строку в файл)
void command_edit(const char *args) {
    if (!args || *args == '\0') {
        printf("\nUsage: edit <filename> <text>\n");
        printf("Appends text to file (with newline)\n");
        return;
    }

    const char *filename = args;
    while (*filename == ' ') filename++;

    const char *text = filename;
    while (*text && *text != ' ') text++;
    if (*text == ' ') {
        text++;
        while (*text == ' ') text++;
    }

    if (*text == '\0') {
        printf("\nUsage: edit <filename> <text>\n");
        return;
    }

    char fname[256];
    int i = 0;
    while (filename[i] && filename[i] != ' ' && i < 255) { fname[i] = filename[i]; i++; }
    fname[i] = '\0';

    uint32_t ino;
    if (lufirafs_lookup(&lufirafs, cwd_inode, fname, &ino) != 0) {
        if (!check_perm(cwd_inode, 0, 1, 1, "edit", fname)) return;
        if (lufirafs_create(&lufirafs, cwd_inode, fname, LUFIRAFS_MODE_FILE,
                             current_process->uid, current_process->gid,
                             LUFIRAFS_DEFAULT_FILE_PERM, &ino) != 0) {
            printf("\nError creating file\n");
            return;
        }
    } else {
        if (!check_perm(ino, 0, 1, 0, "edit", fname)) return;
    }

    int tlen = 0;
    while (text[tlen]) tlen++;

    char *buf = (char *)kmalloc(tlen + 2);
    for (int j = 0; j < tlen; j++) buf[j] = text[j];
    buf[tlen] = '\n';
    buf[tlen + 1] = '\0';

    lufirafs_inode_t inode;
    lufirafs_read_inode(&lufirafs, ino, &inode);
    int result = lufirafs_write(&lufirafs, ino, inode.size, buf, tlen + 1);
    lufirafs_sync(&lufirafs);

    if (result >= 0) {
        printf("\nAppended to '%s' (%d bytes)\n", fname, tlen + 1);
    } else {
        printf("\nError appending to file\n");
    }

    kfree(buf);
}
