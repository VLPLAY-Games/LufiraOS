#include "../commands.h"
#include "drivers/console/console.h"
#include "../shell.h"
#include "fs/fat/fat.h"
#include "system/elf/elf.h"
#include "system/process/process.h"

extern fat_fs_t fatfs;
extern char cwd_path[256];
extern uint32_t cwd_first_cluster;

// ls, cd, pwd, mkdir, rm, touch, cat
void command_ls(const char* flags) {
    int long_fmt = 0;
    if (flags && flags[0] == '-' && flags[1] == 'l') long_fmt = 1;
    fat_dir_t dir;
    if (fat_opendir(&fatfs, cwd_first_cluster, &dir) != 0) {
        printf("\nCannot open directory\n");
        return;
    }
    printf("\n");
    fat_dir_entry_t entry;
    int count = 0;
    while (fat_readdir(&dir, &entry)) {
        char name[13]; int pos = 0;
        for (int j = 0; j < 8 && entry.name[j] != ' '; j++)
            name[pos++] = (char)entry.name[j];
        if (entry.name[8] != ' ') {
            name[pos++] = '.';
            for (int j = 8; j < 11 && entry.name[j] != ' '; j++)
                name[pos++] = (char)entry.name[j];
        }
        name[pos] = '\0';
        if (long_fmt) {
            uint32_t size = read_le32((const uint8_t*)&entry.file_size);
            char type = (entry.attr & 0x10) ? 'd' : '-';
            printf("%c ", type);
            printf("%u ", size);
            printf("%s\n", name);
        } else {
            printf("%s  ", name);
            if (++count % 4 == 0) printf("\n");
        }
    }
    if (!long_fmt && count % 4 != 0) printf("\n");
    fat_closedir(&dir);
}

void command_cd(const char* path) {
    if (!path || !*path) return;

    /* handle ".." */
    if (strcmp(path, "..") == 0) {
        if (cwd_first_cluster == 0 && fatfs.fat_type != 32) {
            /* already root (FAT12/16) */
            return;
        }
        /* read dotdot entry from current directory */
        fat_dir_t dir;
        fat_opendir(&fatfs, cwd_first_cluster, &dir);
        fat_dir_entry_t entry;
        while (fat_readdir(&dir, &entry)) {
            if (entry.name[0] == '.' && entry.name[1] == '.' && entry.name[2] == ' ') {
                uint32_t parent = read_le16((const uint8_t*)&entry.first_cluster_low);
                cwd_first_cluster = parent;
                /* update path string (strip last component) */
                int len = 0; while (cwd_path[len]) len++;
                if (len > 1) {
                    cwd_path[len-1] = '\0';        // remove trailing '/'
                    char *slash = NULL;
                    for (int i = 0; cwd_path[i]; i++)
                        if (cwd_path[i] == '/') slash = &cwd_path[i];
                    if (slash) *slash = '\0';
                    else cwd_path[0] = '/', cwd_path[1] = '\0';
                }
                fat_closedir(&dir);
                return;
            }
        }
        fat_closedir(&dir);
        printf("\ncd: .. not found\n");
        return;
    }

    /* ordinary cd */
    fat_dir_entry_t ent;
    if (fat_find_entry(&fatfs, cwd_first_cluster, path, &ent) == 0) {
        if (!(ent.attr & 0x10)) {
            printf("\ncd: not a directory: %s\n", path);
            return;
        }
        uint32_t new_cluster = read_le16((const uint8_t*)&ent.first_cluster_low);
        cwd_first_cluster = new_cluster;
        /* append name to cwd_path */
        int len = 0; while (cwd_path[len]) len++;
        int plen = 0; while (path[plen]) plen++;
        if (len + 1 + plen < 255) {
            if (cwd_path[0] != '/' || cwd_path[1] != '\0') // not just "/"
                cwd_path[len++] = '/';
            for (int i = 0; path[i]; i++) cwd_path[len++] = path[i];
            cwd_path[len] = '\0';
        }
    } else {
        printf("\ncd: no such directory: %s\n", path);
    }
}

void command_mkdir(const char* name) {
    if (!name || !*name) return;
    int res = fat_mkdir(&fatfs, cwd_first_cluster, name);
    switch (res) {
        case 0:
            printf("\nDirectory created: %s\n", name);
            break;
        case -1:
            printf("\nmkdir: invalid name\n");
            break;
        case -2:
            printf("\nmkdir: '%s' already exists\n", name);
            break;
        case -3:
            printf("\nmkdir: no free directory entry\n");
            break;
        case -4:
            printf("\nmkdir: no free clusters\n");
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
    
    // Проверяем, является ли аргумент "*"
    if (strcmp(name, "*") == 0) {
        // Удаляем все файлы и пустые директории в текущей директории
        fat_dir_t dir;
        if (fat_opendir(&fatfs, cwd_first_cluster, &dir) != 0) {
            printf("\nCannot open directory\n");
            return;
        }
        
        printf("\n");
        fat_dir_entry_t entry;
        int removed_count = 0;
        int error_count = 0;
        
        // Сначала собираем имена всех НЕ-специальных записей
        char names_to_delete[256][13];  // макс 256 файлов
        int name_count = 0;
        
        while (fat_readdir(&dir, &entry)) {
            // Пропускаем . и ..
            if (entry.name[0] == '.' && (entry.name[1] == ' ' || entry.name[1] == '.'))
                continue;
            
            // Пропускаем удаленные записи и LFN
            if (entry.name[0] == 0xE5 || entry.attr == 0x0F)
                continue;
            
            // Формируем имя
            char name_buf[13];
            int pos = 0;
            for (int j = 0; j < 8 && entry.name[j] != ' '; j++)
                name_buf[pos++] = (char)entry.name[j];
            if (entry.name[8] != ' ') {
                name_buf[pos++] = '.';
                for (int j = 8; j < 11 && entry.name[j] != ' '; j++)
                    name_buf[pos++] = (char)entry.name[j];
            }
            name_buf[pos] = '\0';
            
            // Копируем имя в массив для удаления
            strcpy(names_to_delete[name_count], name_buf);
            name_count++;
            
            if (name_count >= 256) break; // Защита от переполнения
        }
        fat_closedir(&dir);
        
        // Теперь удаляем все собранные элементы
        for (int i = 0; i < name_count; i++) {
            int res = fat_rm(&fatfs, cwd_first_cluster, names_to_delete[i]);
            switch (res) {
                case 0:
                    printf("  Removed: %s\n", names_to_delete[i]);
                    removed_count++;
                    break;
                case -4:
                    printf("  Skipped (not empty): %s\n", names_to_delete[i]);
                    error_count++;
                    break;
                default:
                    printf("  Failed to remove: %s (error %d)\n", names_to_delete[i], res);
                    error_count++;
                    break;
            }
        }
        
        printf("\nRemoved %d item(s)", removed_count);
        if (error_count > 0)
            printf(", %d error(s)", error_count);
        printf("\n");
        return;
    }
    
    // Обычное удаление одного файла/директории
    int res = fat_rm(&fatfs, cwd_first_cluster, name);
    switch (res) {
        case 0:
            printf("\nRemoved: %s\n", name);
            break;
        case -1:
            printf("\nrm: invalid name\n");
            break;
        case -2:
            printf("\nrm: '%s' not found\n", name);
            break;
        case -3:
            printf("\nrm: cannot remove '.' or '..'\n");
            break;
        case -4:
            printf("\nrm: directory not empty: %s\n", name);
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
    int res = fat_create_file(&fatfs, cwd_first_cluster, name);
    switch (res) {
        case 0:
            printf("\nFile created: %s\n", name);
            break;
        case -1:
            printf("\ntouch: invalid name\n");
            break;
        case -2:
            printf("\ntouch: '%s' already exists\n", name);
            break;
        case -3:
            printf("\ntouch: no free directory entry\n");
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

    uint32_t fsize;

    if (fat_open(&fatfs, filename, &fsize) == 0) {
        static uint8_t file_buf[4096];

        uint32_t to_read =
            fsize > sizeof(file_buf) ? sizeof(file_buf) : fsize;

        int br = fat_read_file(
            &fatfs,
            filename,
            file_buf,
            to_read
        );

        if (br >= 0) {
            printf("\n--- %s (%u bytes) ---\n", filename, fsize);

            for (int i = 0; i < br; i++) {
                put_char(file_buf[i]);
            }

            printf("\n--- end ---\n");
        } else {
            printf("\nError reading file.\n");
        }
    } else {
        printf("\nFile not found: %s\n", filename);
    }
}


// run - запуск ELF файла
void command_run(const char *filename) {
    if (!filename || *filename == '\0') {
        printf("\nUsage: run <filename>\n");
        printf("Example: run hello.elf\n");
        return;
    }
    
    // Открываем файл
    uint32_t fsize;
    if (fat_open(&fatfs, filename, &fsize) != 0) {
        printf("\nFile not found: %s\n", filename);
        return;
    }
    
    // Выделяем буфер для файла
    uint8_t *file_buf = (uint8_t *)kmalloc(fsize);
    if (!file_buf) {
        printf("\nNot enough memory to load %s (%u bytes)\n", filename, fsize);
        return;
    }
    
    // Читаем файл
    int br = fat_read_file(&fatfs, filename, file_buf, fsize);
    if (br <= 0) {
        printf("\nError reading file: %s\n", filename);
        kfree(file_buf);
        return;
    }
    
    printf("\nLoading ELF: %s (%u bytes)...\n", filename, fsize);
    
    // Запускаем ELF
    if (elf_exec(file_buf, fsize, filename) == 0) {
        printf("Process started!\n");
    }
    
    // Не освобождаем буфер - он используется процессом
}

// write - запись в файл
void command_write(const char *filename) {
    if (!filename || *filename == '\0') {
        printf("\nUsage: write <filename> <text>\n");
        printf("Example: write test.txt Hello World\n");
        return;
    }
    
    // Ищем аргументы (текст для записи)
    const char *text = filename;
    while (*text && *text != ' ') text++;
    if (*text == ' ') {
        text++;  // Пропускаем пробел
        while (*text == ' ') text++;
    }
    
    if (*text == '\0') {
        printf("\nUsage: write <filename> <text>\n");
        return;
    }
    
    // Вычисляем длину текста
    int len = 0;
    while (text[len]) len++;
    
    // Пытаемся открыть файл чтобы проверить существует ли
    uint32_t fsize;
    int exists = (fat_open(&fatfs, filename, &fsize) == 0);
    
    int result;
    if (exists) {
        // Перезаписываем
        result = fat_write_file(&fatfs, filename, text, len);
        printf("\nFile '%s' overwritten (%d bytes)\n", filename, len);
    } else {
        // Создаём новый
        fat_create_file(&fatfs, 0, filename);
        result = fat_write_file(&fatfs, filename, text, len);
        printf("\nFile '%s' created and written (%d bytes)\n", filename, len);
    }
    
    if (result != 0) {
        printf("Error writing file!\n");
    }
}

// cp - копирование файла
void command_cp(const char *args) {
    if (!args || *args == '\0') {
        printf("\nUsage: cp <source> <destination>\n");
        return;
    }
    
    // Находим исходный файл
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
    
    // Копируем имена
    char src_name[256], dst_name[256];
    int i = 0;
    while (src[i] && src[i] != ' ' && i < 255) { src_name[i] = src[i]; i++; }
    src_name[i] = '\0';
    
    i = 0;
    while (dst[i] && dst[i] != ' ' && i < 255) { dst_name[i] = dst[i]; i++; }
    dst_name[i] = '\0';
    
    // Читаем исходный файл
    uint32_t fsize;
    if (fat_open(&fatfs, src_name, &fsize) != 0) {
        printf("\ncp: source file not found: %s\n", src_name);
        return;
    }
    
    uint8_t *buf = (uint8_t *)kmalloc(fsize);
    if (!buf) {
        printf("\ncp: not enough memory\n");
        return;
    }
    
    int br = fat_read_file(&fatfs, src_name, buf, fsize);
    if (br <= 0) {
        printf("\ncp: error reading source file\n");
        kfree(buf);
        return;
    }
    
    // Записываем в целевой файл
    fat_create_file(&fatfs, 0, dst_name);
    int result = fat_write_file(&fatfs, dst_name, buf, fsize);
    
    if (result == 0) {
        printf("\nCopied '%s' to '%s' (%u bytes)\n", src_name, dst_name, fsize);
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
    
    // Копируем файл
    uint32_t fsize;
    if (fat_open(&fatfs, src_name, &fsize) != 0) {
        printf("\nmv: source file not found: %s\n", src_name);
        return;
    }
    
    uint8_t *buf = (uint8_t *)kmalloc(fsize);
    if (!buf) {
        printf("\nmv: not enough memory\n");
        return;
    }
    
    int br = fat_read_file(&fatfs, src_name, buf, fsize);
    if (br <= 0) {
        printf("\nmv: error reading source file\n");
        kfree(buf);
        return;
    }
    
    fat_create_file(&fatfs, 0, dst_name);
    fat_write_file(&fatfs, dst_name, buf, fsize);
    
    // Удаляем исходный
    fat_rm(&fatfs, 0, src_name);
    
    printf("\nMoved '%s' to '%s' (%u bytes)\n", src_name, dst_name, fsize);
    
    kfree(buf);
}

// rename - переименование
void command_rename(const char *args) {
    command_mv(args);  // rename = mv
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
    
    // Проверяем существует ли файл
    uint32_t fsize;
    if (fat_open(&fatfs, fname, &fsize) != 0) {
        fat_create_file(&fatfs, 0, fname);
    }
    
    // Создаём строку с текстом + \n
    int tlen = 0;
    while (text[tlen]) tlen++;
    
    char *buf = (char *)kmalloc(tlen + 2);
    for (int j = 0; j < tlen; j++) buf[j] = text[j];
    buf[tlen] = '\n';
    buf[tlen + 1] = '\0';
    
    int result = fat_append_file(&fatfs, fname, buf, tlen + 1);
    
    if (result == 0) {
        printf("\nAppended to '%s' (%d bytes)\n", fname, tlen + 1);
    } else {
        printf("\nError appending to file\n");
    }
    
    kfree(buf);
}
