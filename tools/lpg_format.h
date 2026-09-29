// lpg_format.h — формат пакета LufiraOS (.lpg), v0.7 план, этап 2.
//
// Общий заголовок для ДВУХ независимых потребителей: упаковщика
// (tools/lpg_pack.c — хостовый инструмент, обычный gcc) и dlpg
// (userspace/base/dlpg.c — freestanding ELF-программа поверх libc/). Оба
// собираются РАЗНЫМИ тулчейн-флагами, поэтому этот файл использует только
// <stdint.h> — он есть в обоих мирах (у freestanding-сборки свой
// собственный, встроенный в gcc, см. libc/crt0.S) — никаких host-only
// заголовков (stdio.h и т.п.) сюда не тащим.
//
// Свой ручной бинарный формат, а не готовый tar/zip — в этом проекте не
// подключено ни одной архивной библиотеки нигде (ни в ядре, ни в
// userspace), и подключать её просто для .lpg — отдельная, не нужная тут
// задача. Тот же стиль ручного побайтового формата, что уже у
// lufirafs_format.h.
//
// Раскладка файла на диске:
//   [lpg_header_t]
//   [lpg_dependency_t] * header.dep_count
//   [lpg_file_entry_t] * header.file_count
//   [данные файлов, конкатенированы; на каждый файл — lpg_file_entry_t.offset
//    указывает АБСОЛЮТНОЕ смещение от начала .lpg, размер — .size]
#pragma once

#include <stdint.h>

#define LPG_MAGIC "LPG1"
#define LPG_MAGIC_LEN 4

#define LPG_NAME_MAX 32
#define LPG_PATH_MAX 64

// Разумные потолки на случай повреждённого/чужого файла — dlpg отказывается
// парсить заголовок, заявляющий больше, ещё до попытки что-либо выделить
// под dep_count/file_count записей (защита от integer-overflow в
// malloc(count * sizeof(entry))).
#define LPG_MAX_DEPS  16
#define LPG_MAX_FILES 64

typedef enum {
    LPG_CATEGORY_BASE = 0,
    LPG_CATEGORY_USER = 1,
} lpg_category_t;

// Простое (major,minor,patch) вместо строки — сравнение зависимостей
// ("установлена версия >= минимально требуемой") тогда обычная числовая
// проверка по кортежу, без парсинга строк на стороне dlpg.
typedef struct __attribute__((packed)) {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
} lpg_version_t;

// Возвращает 1, если a >= b (лексикографически по major,minor,patch).
static inline int lpg_version_gte(lpg_version_t a, lpg_version_t b) {
    if (a.major != b.major) return a.major > b.major;
    if (a.minor != b.minor) return a.minor > b.minor;
    return a.patch >= b.patch;
}

typedef struct __attribute__((packed)) {
    char magic[LPG_MAGIC_LEN];      // "LPG1", без завершающего NUL
    char name[LPG_NAME_MAX];        // NUL-terminated, паддинг нулями
    lpg_version_t version;
    uint8_t category;               // lpg_category_t
    uint8_t reserved[1];
    uint32_t dep_count;
    uint32_t file_count;
} lpg_header_t;

typedef struct __attribute__((packed)) {
    char name[LPG_NAME_MAX];
    lpg_version_t min_version;
} lpg_dependency_t;

typedef struct __attribute__((packed)) {
    char path[LPG_PATH_MAX];        // абсолютный путь установки, напр. "/bin/du.elf"
    uint32_t size;
    uint32_t mode;                  // unix-права, напр. 0755 (восьмеричное 755 = 493 десятичное)
    uint32_t offset;                // абсолютное смещение данных этого файла от начала .lpg
} lpg_file_entry_t;
