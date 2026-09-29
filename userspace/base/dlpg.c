// dlpg.c — установщик пакетов LufiraOS (v0.7 план, этап 2). Base-пакет
// (сам является userspace ELF), но не требует НИ ОДНОГО нового syscall'а —
// вся логика (парсинг .lpg, проверка зависимостей, распаковка, БД
// установленных пакетов) поверх уже существующих OPEN/READ/WRITE/CLOSE/
// SEEK/CHMOD/MKDIR/UNLINK.
//
// Подкоманды:
//   dlpg install <path.lpg>   — установить новый пакет (отказ, если уже есть)
//   dlpg update  <path.lpg>   — установить/обновить (не отказывает, если уже есть)
//   dlpg list                 — показать установленные пакеты
//   dlpg remove  <name>       — удалить пакет и все его файлы
//
// БД установленных пакетов — /etc/packages/installed, построчно
// "имя:major.minor.patch:category" (тот же приём, что уже /etc/passwd,
// /etc/group — kernel/system/users/users.c). Список файлов каждого
// пакета — свой файл /etc/packages/<имя>.files (по одному абсолютному
// пути на строку), нужен только remove'у, чтобы знать, что удалять.
//
// Сборка (отличается от других userspace-программ ОДНИМ дополнительным
// -I — общий формат .lpg лежит в tools/, не в libc/include):
//   gcc $FLAGS -Itools -c userspace/base/dlpg.c -o dlpg.o
//   ld ... -o dlpg.elf crt0.o dlpg.o string.o malloc.o printf.o stdlib.o

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lufira/syscall.h>
#include "lpg_format.h"

#define INSTALLED_DB   "/etc/packages/installed"
#define PACKAGES_DIR   "/etc/packages"
#define MAX_INSTALLED  32

typedef struct {
    char name[LPG_NAME_MAX];
    lpg_version_t version;
    uint8_t category;
} installed_entry_t;

/* ===================== мелкие ручные парсеры =====================
 * В этой freestanding-libc нет ни atoi(), ни sscanf() (см. libc/include/
 * stdlib.h) — те же ручные циклы по цифрам, что уже используются во всём
 * остальном ядре (kernel/lib/string.c: atoi()/hex_to_int()).
 */
static unsigned int parse_uint_local(const char *s, int *consumed) {
    unsigned int v = 0;
    int i = 0;
    while (s[i] >= '0' && s[i] <= '9') { v = v * 10 + (unsigned int)(s[i] - '0'); i++; }
    if (consumed) *consumed = i;
    return v;
}

static lpg_version_t parse_version_str(const char *s) {
    lpg_version_t v = {0, 0, 0};
    int c;
    v.major = (uint16_t)parse_uint_local(s, &c); s += c;
    if (*s == '.') s++;
    v.minor = (uint16_t)parse_uint_local(s, &c); s += c;
    if (*s == '.') s++;
    v.patch = (uint16_t)parse_uint_local(s, &c);
    return v;
}

// Режет line по ':' на месте (как split_fields() в users.c).
static int split_fields(char *line, char *fields[], int max_fields) {
    int count = 0;
    char *p = line;
    fields[count++] = p;
    while (*p && count < max_fields) {
        if (*p == ':') { *p = '\0'; p++; fields[count++] = p; }
        else p++;
    }
    return count;
}

/* ===================== файловые хелперы через syscalls ===================== */

static char *read_whole_file(const char *path, long *out_len) {
    long fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;
    long size = sys_lseek((int)fd, 0, SEEK_END);
    sys_lseek((int)fd, 0, SEEK_SET);
    if (size < 0) { sys_close((int)fd); return NULL; }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { sys_close((int)fd); return NULL; }

    long total = 0;
    while (total < size) {
        long n = sys_read((int)fd, buf + total, (unsigned long)(size - total));
        if (n <= 0) break;
        total += n;
    }
    sys_close((int)fd);
    buf[total] = '\0';
    if (out_len) *out_len = total;
    return buf;
}

static int write_whole_file(const char *path, const void *data, long len) {
    long fd = sys_open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return -1;
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

/* ===================== БД установленных пакетов ===================== */

// Возвращает число прочитанных записей (0, если файла ещё нет — свежая
// система без единого установленного пакета).
static int load_installed(installed_entry_t *out) {
    long len;
    char *content = read_whole_file(INSTALLED_DB, &len);
    if (!content) return 0;

    int count = 0;
    char *line = content;
    while (*line && count < MAX_INSTALLED) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (*line) {
            char *fields[3];
            if (split_fields(line, fields, 3) == 3) {
                memset(&out[count], 0, sizeof(out[count]));
                strncpy(out[count].name, fields[0], LPG_NAME_MAX - 1);
                out[count].version = parse_version_str(fields[1]);
                out[count].category = (strcmp(fields[2], "base") == 0) ? LPG_CATEGORY_BASE : LPG_CATEGORY_USER;
                count++;
            }
        }
        line = nl ? nl + 1 : line + strlen(line);
    }
    free(content);
    return count;
}

static int save_installed(installed_entry_t *entries, int count) {
    char buf[MAX_INSTALLED * 64];
    int pos = 0;
    for (int i = 0; i < count; i++) {
        int n = 0;
        // Собираем строку вручную (нет snprintf в этой freestanding libc) —
        // тот же приём, что уже у users.c/build_user_line().
        const char *cat = (entries[i].category == LPG_CATEGORY_BASE) ? "base" : "user";
        char line[128];
        // "name:major.minor.patch:category\n"
        int p2 = 0;
        const char *s = entries[i].name;
        while (*s && p2 < (int)sizeof(line) - 1) line[p2++] = *s++;
        line[p2++] = ':';

        unsigned int parts[3] = {entries[i].version.major, entries[i].version.minor, entries[i].version.patch};
        for (int k = 0; k < 3; k++) {
            char digits[8];
            int dn = 0;
            unsigned int v = parts[k];
            if (v == 0) digits[dn++] = '0';
            while (v > 0) { digits[dn++] = (char)('0' + v % 10); v /= 10; }
            while (dn > 0 && p2 < (int)sizeof(line) - 1) line[p2++] = digits[--dn];
            if (k < 2 && p2 < (int)sizeof(line) - 1) line[p2++] = '.';
        }
        line[p2++] = ':';
        s = cat;
        while (*s && p2 < (int)sizeof(line) - 1) line[p2++] = *s++;
        line[p2++] = '\n';
        line[p2] = '\0';
        n = p2;

        if (pos + n < (int)sizeof(buf)) {
            memcpy(buf + pos, line, (size_t)n);
            pos += n;
        }
    }
    return write_whole_file(INSTALLED_DB, buf, pos);
}

static int find_installed(installed_entry_t *entries, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) return i;
    }
    return -1;
}

/* ===================== install/update ===================== */

static int do_install(const char *lpg_path, int allow_existing) {
    long len;
    uint8_t *buf = (uint8_t *)read_whole_file(lpg_path, &len);
    if (!buf) { printf("dlpg: cannot open %s\n", lpg_path); return 1; }

    if (len < (long)sizeof(lpg_header_t)) { printf("dlpg: %s is too small to be a .lpg\n", lpg_path); free(buf); return 1; }

    lpg_header_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (memcmp(hdr.magic, LPG_MAGIC, LPG_MAGIC_LEN) != 0) {
        printf("dlpg: %s is not a valid .lpg (bad magic)\n", lpg_path);
        free(buf);
        return 1;
    }
    if (hdr.dep_count > LPG_MAX_DEPS || hdr.file_count > LPG_MAX_FILES || hdr.file_count == 0) {
        printf("dlpg: %s has an invalid header\n", lpg_path);
        free(buf);
        return 1;
    }

    installed_entry_t installed[MAX_INSTALLED];
    int installed_count = load_installed(installed);

    int existing_idx = find_installed(installed, installed_count, hdr.name);
    if (existing_idx >= 0 && !allow_existing) {
        printf("dlpg: package '%s' is already installed (use 'update' to reinstall)\n", hdr.name);
        free(buf);
        return 1;
    }

    // Проверка зависимостей — ДО того, как что-либо распаковано на диск,
    // чтобы отказ не оставлял систему в частично установленном состоянии.
    lpg_dependency_t *deps = (lpg_dependency_t *)(buf + sizeof(lpg_header_t));
    for (uint32_t i = 0; i < hdr.dep_count; i++) {
        int dep_idx = find_installed(installed, installed_count, deps[i].name);
        if (dep_idx < 0) {
            printf("dlpg: missing dependency '%s'\n", deps[i].name);
            free(buf);
            return 1;
        }
        if (!lpg_version_gte(installed[dep_idx].version, deps[i].min_version)) {
            printf("dlpg: dependency '%s' too old (need >= %u.%u.%u, have %u.%u.%u)\n",
                   deps[i].name, deps[i].min_version.major, deps[i].min_version.minor, deps[i].min_version.patch,
                   installed[dep_idx].version.major, installed[dep_idx].version.minor, installed[dep_idx].version.patch);
            free(buf);
            return 1;
        }
    }

    lpg_file_entry_t *files = (lpg_file_entry_t *)(buf + sizeof(lpg_header_t) + hdr.dep_count * sizeof(lpg_dependency_t));

    // Распаковка + сборка receipt-файла (список установленных путей) для remove().
    char receipt[LPG_MAX_FILES * (LPG_PATH_MAX + 1)];
    int receipt_pos = 0;

    for (uint32_t i = 0; i < hdr.file_count; i++) {
        const uint8_t *data = buf + files[i].offset;
        if (write_whole_file(files[i].path, data, (long)files[i].size) != 0) {
            printf("dlpg: failed to write %s\n", files[i].path);
            free(buf);
            return 1;
        }
        sys_chmod(files[i].path, (int)files[i].mode);

        int plen = (int)strlen(files[i].path);
        if (receipt_pos + plen + 1 < (int)sizeof(receipt)) {
            memcpy(receipt + receipt_pos, files[i].path, (size_t)plen);
            receipt_pos += plen;
            receipt[receipt_pos++] = '\n';
        }
        printf("dlpg: installed %s (%u bytes)\n", files[i].path, files[i].size);
    }

    char receipt_path[LPG_NAME_MAX + 32];
    int rp = 0;
    const char *prefix = PACKAGES_DIR "/";
    while (prefix[rp]) { receipt_path[rp] = prefix[rp]; rp++; }
    int ni = 0;
    while (hdr.name[ni] && rp < (int)sizeof(receipt_path) - 8) receipt_path[rp++] = hdr.name[ni++];
    const char *suffix = ".files";
    int si = 0;
    while (suffix[si]) receipt_path[rp++] = suffix[si++];
    receipt_path[rp] = '\0';
    write_whole_file(receipt_path, receipt, receipt_pos);

    if (existing_idx >= 0) {
        installed[existing_idx].version = hdr.version;
        installed[existing_idx].category = hdr.category;
    } else if (installed_count < MAX_INSTALLED) {
        memset(&installed[installed_count], 0, sizeof(installed[installed_count]));
        strncpy(installed[installed_count].name, hdr.name, LPG_NAME_MAX - 1);
        installed[installed_count].version = hdr.version;
        installed[installed_count].category = hdr.category;
        installed_count++;
    } else {
        printf("dlpg: installed-package table full (max %d)\n", MAX_INSTALLED);
        free(buf);
        return 1;
    }
    save_installed(installed, installed_count);

    printf("dlpg: %s '%s' v%u.%u.%u\n", existing_idx >= 0 ? "updated" : "installed",
           hdr.name, hdr.version.major, hdr.version.minor, hdr.version.patch);
    free(buf);
    return 0;
}

/* ===================== list/remove ===================== */

static int do_list(void) {
    installed_entry_t installed[MAX_INSTALLED];
    int count = load_installed(installed);
    if (count == 0) { printf("No packages installed.\n"); return 0; }

    for (int i = 0; i < count; i++) {
        printf("%s %u.%u.%u (%s)\n", installed[i].name,
               installed[i].version.major, installed[i].version.minor, installed[i].version.patch,
               installed[i].category == LPG_CATEGORY_BASE ? "base" : "user");
    }
    return 0;
}

static int do_remove(const char *name) {
    installed_entry_t installed[MAX_INSTALLED];
    int count = load_installed(installed);

    int idx = find_installed(installed, count, name);
    if (idx < 0) { printf("dlpg: package '%s' is not installed\n", name); return 1; }

    char receipt_path[LPG_NAME_MAX + 32];
    int rp = 0;
    const char *prefix = PACKAGES_DIR "/";
    while (prefix[rp]) { receipt_path[rp] = prefix[rp]; rp++; }
    int ni = 0;
    while (name[ni] && rp < (int)sizeof(receipt_path) - 8) receipt_path[rp++] = name[ni++];
    const char *suffix = ".files";
    int si = 0;
    while (suffix[si]) receipt_path[rp++] = suffix[si++];
    receipt_path[rp] = '\0';

    long len;
    char *content = read_whole_file(receipt_path, &len);
    if (content) {
        char *line = content;
        while (*line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (*line) {
                sys_unlink(line);
                printf("dlpg: removed %s\n", line);
            }
            line = nl ? nl + 1 : line + strlen(line);
        }
        free(content);
    }
    sys_unlink(receipt_path);

    for (int i = idx; i < count - 1; i++) installed[i] = installed[i + 1];
    count--;
    save_installed(installed, count);

    printf("dlpg: removed package '%s'\n", name);
    return 0;
}

/* ===================== main ===================== */

static void print_usage(void) {
    printf("Usage: dlpg install <path.lpg>\n");
    printf("       dlpg update  <path.lpg>\n");
    printf("       dlpg list\n");
    printf("       dlpg remove  <name>\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(); return 1; }

    // Игнорируем результат — успех, если директории ещё не было и она
    // создалась, успех и если она уже была (sys_mkdir() тогда вернёт
    // ошибку "уже существует", которую здесь незачем отдельно различать).
    sys_mkdir(PACKAGES_DIR, 0755);

    const char *subcmd = argv[1];
    if (strcmp(subcmd, "install") == 0 || strcmp(subcmd, "update") == 0) {
        if (argc < 3) { print_usage(); return 1; }
        return do_install(argv[2], strcmp(subcmd, "update") == 0);
    } else if (strcmp(subcmd, "list") == 0) {
        return do_list();
    } else if (strcmp(subcmd, "remove") == 0) {
        if (argc < 3) { print_usage(); return 1; }
        return do_remove(argv[2]);
    }

    print_usage();
    return 1;
}
