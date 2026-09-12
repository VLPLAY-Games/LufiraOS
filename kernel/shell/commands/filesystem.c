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
#include "lib/string.h"

#define LS_COLOR_DIR  COLOR_LIGHT_BLUE
#define LS_COLOR_FILE COLOR_WHITE

extern lufirafs_t lufirafs;
extern char cwd_path[256];
extern uint32_t cwd_inode;

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

    printf("\n");
    lufirafs_dirent_t entry;
    int count = 0;
    while (lufirafs_readdir(&dir, &entry) == 0) {
        if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0)
            continue;

        lufirafs_inode_t inode;
        lufirafs_read_inode(&lufirafs, entry.inode, &inode);
        int is_dir = (inode.mode == LUFIRAFS_MODE_DIR);

        set_foreground_color(is_dir ? LS_COLOR_DIR : LS_COLOR_FILE);

        if (long_fmt) {
            printf("%c %u ", is_dir ? 'd' : '-', inode.size);
            printf("%s\n", entry.name);
        } else {
            printf("%s  ", entry.name);
            if (++count % 4 == 0) printf("\n");
        }
    }
    set_foreground_color(LOG_COLOR_INFO);
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

    if (strcmp(path, "..") == 0) {
        if (cwd_inode != lufirafs.sb.root_inode) {
            int len = 0; while (cwd_path[len]) len++;
            if (len > 1) {
                cwd_path[len - 1] = '\0'; // убираем хвостовой '/'
                char *slash = NULL;
                for (int i = 0; cwd_path[i]; i++)
                    if (cwd_path[i] == '/') slash = &cwd_path[i];
                if (slash) *slash = '\0';
                else { cwd_path[0] = '/'; cwd_path[1] = '\0'; }
            }
        }
    } else if (strcmp(path, ".") != 0) {
        int len = 0; while (cwd_path[len]) len++;
        int plen = 0; while (path[plen]) plen++;
        if (len + 1 + plen < 255) {
            if (cwd_path[0] != '/' || cwd_path[1] != '\0') // не просто "/"
                cwd_path[len++] = '/';
            for (int i = 0; path[i]; i++) cwd_path[len++] = path[i];
            cwd_path[len] = '\0';
        }
    }

    cwd_inode = new_inode;
}

void command_mkdir(const char* name) {
    if (!name || !*name) return;

    uint32_t out_ino;
    int res = lufirafs_create(&lufirafs, cwd_inode, name, LUFIRAFS_MODE_DIR, &out_ino);
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
    uint32_t out_ino;
    int res = lufirafs_create(&lufirafs, cwd_inode, name, LUFIRAFS_MODE_FILE, &out_ino);
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

    if (do_exec(filename) != 0) {
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

    if (!exists) {
        if (lufirafs_create(&lufirafs, cwd_inode, filename, LUFIRAFS_MODE_FILE, &ino) != 0) {
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

    uint32_t dst_ino;
    if (lufirafs_lookup(&lufirafs, cwd_inode, dst_name, &dst_ino) != 0) {
        if (lufirafs_create(&lufirafs, cwd_inode, dst_name, LUFIRAFS_MODE_FILE, &dst_ino) != 0) {
            printf("\ncp: error creating destination file\n");
            kfree(buf);
            return;
        }
    } else {
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

    uint32_t src_ino;
    if (lufirafs_lookup(&lufirafs, cwd_inode, src_name, &src_ino) != 0) {
        printf("\nmv: source file not found: %s\n", src_name);
        return;
    }

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

    uint32_t dst_ino;
    if (lufirafs_lookup(&lufirafs, cwd_inode, dst_name, &dst_ino) != 0) {
        lufirafs_create(&lufirafs, cwd_inode, dst_name, LUFIRAFS_MODE_FILE, &dst_ino);
    } else {
        lufirafs_truncate(&lufirafs, dst_ino, 0);
    }
    lufirafs_write(&lufirafs, dst_ino, 0, buf, src_inode.size);

    lufirafs_unlink(&lufirafs, cwd_inode, src_name);
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
        if (lufirafs_create(&lufirafs, cwd_inode, fname, LUFIRAFS_MODE_FILE, &ino) != 0) {
            printf("\nError creating file\n");
            return;
        }
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
