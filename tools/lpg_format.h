// lpg_format.h — формат пакета LufiraOS (.lpg).
//
// Общий заголовок для двух независимых потребителей: упаковщика
// (tools/lpg_pack.c — хостовый gcc) и dlpg (userspace/base/dlpg.c —
// freestanding ELF поверх libc/). Собираются разными тулчейн-флагами,
// поэтому файл использует только <stdint.h> — он есть в обоих мирах.
//
// Свой ручной бинарный формат, а не tar/zip — в проекте нигде не
// подключено ни одной архивной библиотеки, и тащить её ради .lpg незачем.
// Тот же стиль, что уже у lufirafs_format.h.
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

// Потолки на случай повреждённого файла — dlpg отказывается парсить
// заголовок, заявляющий больше, до попытки выделить память под записи
// (защита от integer-overflow в malloc(count * sizeof(entry))).
#define LPG_MAX_DEPS  16
#define LPG_MAX_FILES 64

typedef enum {
    LPG_CATEGORY_BASE = 0,
    LPG_CATEGORY_USER = 1,
    LPG_CATEGORY_GAMES = 2,
} lpg_category_t;

// (major,minor,patch) вместо строки — сравнение версий тогда обычная
// числовая проверка по кортежу, без парсинга строк на стороне dlpg.
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

// Возвращает 1, если a строго больше b — нужен dlpg upgrade, чтобы отличить
// "в репозитории версия новее" от "уже такая же" (gte() одного тут
// недостаточно: gte(remote,local) истинно и при равных версиях).
static inline int lpg_version_gt(lpg_version_t a, lpg_version_t b) {
    if (a.major != b.major) return a.major > b.major;
    if (a.minor != b.minor) return a.minor > b.minor;
    return a.patch > b.patch;
}

typedef struct __attribute__((packed)) {
    char magic[LPG_MAGIC_LEN];      // "LPG1", без завершающего NUL
    char name[LPG_NAME_MAX];        // NUL-terminated, паддинг нулями
    lpg_version_t version;
    uint8_t category;               // lpg_category_t
    uint8_t reserved[1];
    uint32_t dep_count;
    uint32_t file_count;
    // Минимальная версия ЯДРА, под которую собран пакет — build.py пишет
    // туда текущую KERNEL_VERSION, а не версию из manifest-а: это факт
    // ("с каким ABI/набором syscall'ов слинкован"), а не зависимость,
    // выбираемая автором пакета. dlpg сверяет её с версией реально
    // запущенного ядра той же lpg_version_gte(). Добавлено в конец —
    // обратно совместимое расширение: старый lpg_pack/dlpg просто не
    // читает/не пишет это поле.
    lpg_version_t min_kernel_version;
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
