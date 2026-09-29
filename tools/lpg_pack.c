// lpg_pack.c — хостовый упаковщик .lpg (v0.7 план, этап 2). Тот же стиль,
// что и tools/mkfs_lufirafs.c: обычный hosted C, без внешних библиотек,
// собирается штатным gcc для машины разработчика (не freestanding).
//
// Берёт манифест (простой текстовый файл, см. пример ниже) и пишет один
// .lpg по формату из tools/lpg_format.h.
//
// Формат манифеста:
//   name=du
//   version=1.0.0
//   category=user            (base|user)
//   depends=                 (пусто, либо "имя:1.0.0,имя2:0.2.0")
//   [files]
//   userspace/user/du.elf /bin/du.elf 755
//   ещё-файл-с-хоста /абсолютный/путь/назначения права-восьмеричные
//
// Использование:
//   lpg_pack <manifest> <output.lpg>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lpg_format.h"

// strncpy() триггерит -Wstringop-truncation даже когда вызывающий явно
// терминирует NUL'ом сам сразу после — GCC не видит связи между двумя
// отдельными операторами. memcpy + явный NUL здесь и есть эта связь в
// одном месте, без ложного срабатывания.
static void safe_copy(char *dst, const char *src, size_t cap) {
    size_t len = strlen(src);
    if (len > cap - 1) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void die(const char *msg) {
    fprintf(stderr, "lpg_pack: %s\n", msg);
    exit(1);
}

static void trim(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ')) s[--len] = '\0';
}

static lpg_version_t parse_version(const char *s) {
    lpg_version_t v = {0, 0, 0};
    unsigned int maj = 0, min = 0, pat = 0;
    sscanf(s, "%u.%u.%u", &maj, &min, &pat);
    v.major = (uint16_t)maj;
    v.minor = (uint16_t)min;
    v.patch = (uint16_t)pat;
    return v;
}

// Разбирает "имя1:1.0.0,имя2:0.2.0" в массив зависимостей (до
// LPG_MAX_DEPS). Возвращает количество разобранных записей.
static uint32_t parse_depends(const char *s, lpg_dependency_t *out) {
    uint32_t count = 0;
    char buf[1024];
    safe_copy(buf, s, sizeof(buf));

    char *tok = strtok(buf, ",");
    while (tok && count < LPG_MAX_DEPS) {
        while (*tok == ' ') tok++;
        char *colon = strchr(tok, ':');
        if (colon) {
            *colon = '\0';
            memset(&out[count], 0, sizeof(out[count]));
            safe_copy(out[count].name, tok, LPG_NAME_MAX);
            out[count].min_version = parse_version(colon + 1);
            count++;
        }
        tok = strtok(NULL, ",");
    }
    return count;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <manifest> <output.lpg>\n", argv[0]);
        return 1;
    }

    FILE *mf = fopen(argv[1], "r");
    if (!mf) die("cannot open manifest");

    char name[LPG_NAME_MAX] = {0};
    lpg_version_t version = {0, 0, 0};
    uint8_t category = LPG_CATEGORY_USER;
    lpg_dependency_t deps[LPG_MAX_DEPS];
    uint32_t dep_count = 0;

    char src_paths[LPG_MAX_FILES][512];
    lpg_file_entry_t files[LPG_MAX_FILES];
    uint32_t file_count = 0;

    char line[1024];
    int in_files = 0;
    while (fgets(line, sizeof(line), mf)) {
        trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;

        if (strcmp(line, "[files]") == 0) { in_files = 1; continue; }

        if (!in_files) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            const char *key = line;
            const char *value = eq + 1;

            if (strcmp(key, "name") == 0) {
                safe_copy(name, value, LPG_NAME_MAX);
            } else if (strcmp(key, "version") == 0) {
                version = parse_version(value);
            } else if (strcmp(key, "category") == 0) {
                category = (strcmp(value, "base") == 0) ? LPG_CATEGORY_BASE : LPG_CATEGORY_USER;
            } else if (strcmp(key, "depends") == 0) {
                dep_count = parse_depends(value, deps);
            }
        } else {
            if (file_count >= LPG_MAX_FILES) die("too many files (LPG_MAX_FILES)");

            char src[512], dst[LPG_PATH_MAX];
            unsigned int mode = 0644;
            int n = sscanf(line, "%511s %63s %o", src, dst, &mode);
            if (n < 2) die("malformed [files] line (expected: <src> <dst> [mode])");

            safe_copy(src_paths[file_count], src, sizeof(src_paths[file_count]));
            memset(&files[file_count], 0, sizeof(files[file_count]));
            safe_copy(files[file_count].path, dst, LPG_PATH_MAX);
            files[file_count].mode = mode;
            file_count++;
        }
    }
    fclose(mf);

    if (name[0] == '\0') die("manifest missing 'name='");
    if (file_count == 0) die("manifest has no [files]");

    // Читаем каждый исходный файл целиком, считаем итоговое смещение в
    // .lpg (после заголовка + всех записей о зависимостях/файлах).
    uint8_t *file_data[LPG_MAX_FILES];
    uint32_t data_start = (uint32_t)(sizeof(lpg_header_t) +
                                      dep_count * sizeof(lpg_dependency_t) +
                                      file_count * sizeof(lpg_file_entry_t));
    uint32_t running_offset = data_start;

    for (uint32_t i = 0; i < file_count; i++) {
        FILE *f = fopen(src_paths[i], "rb");
        if (!f) { fprintf(stderr, "lpg_pack: cannot open source file %s\n", src_paths[i]); return 1; }
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsize < 0) die("ftell failed");

        file_data[i] = (uint8_t *)malloc((size_t)fsize > 0 ? (size_t)fsize : 1);
        if (fsize > 0 && fread(file_data[i], 1, (size_t)fsize, f) != (size_t)fsize) die("short read on source file");
        fclose(f);

        files[i].size = (uint32_t)fsize;
        files[i].offset = running_offset;
        running_offset += (uint32_t)fsize;
    }

    lpg_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, LPG_MAGIC, LPG_MAGIC_LEN);
    safe_copy(hdr.name, name, LPG_NAME_MAX);
    hdr.version = version;
    hdr.category = category;
    hdr.dep_count = dep_count;
    hdr.file_count = file_count;

    FILE *out = fopen(argv[2], "wb");
    if (!out) die("cannot open output file");
    fwrite(&hdr, sizeof(hdr), 1, out);
    if (dep_count > 0) fwrite(deps, sizeof(lpg_dependency_t), dep_count, out);
    fwrite(files, sizeof(lpg_file_entry_t), file_count, out);
    for (uint32_t i = 0; i < file_count; i++) {
        fwrite(file_data[i], 1, files[i].size, out);
        free(file_data[i]);
    }
    fclose(out);

    printf("lpg_pack: wrote %s (%s v%u.%u.%u, %s, %u dep(s), %u file(s))\n",
           argv[2], name, version.major, version.minor, version.patch,
           category == LPG_CATEGORY_BASE ? "base" : "user", dep_count, file_count);
    return 0;
}
