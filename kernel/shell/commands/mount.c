// Команды mount/unmount — активируют FAT-драйвер (fs/fat/fat.c) для USB-
// флешек. VFS сегодня не умеет монтировать вторую ФС (нет реестра
// filesystem_t, всё жёстко завязано на LufiraFS), поэтому это не настоящий
// VFS-mount, а отдельная подсистема поверх N параллельных fat_fs_t
// контекстов (см. MAX_MOUNTS ниже), работающая напрямую с xhci_msd_*/fat_*,
// в стороне от VFS — как usbinfo/usbread/usbwrite (usb.c). fat_init()
// ожидает образ ФС целиком в RAM, так что mount читает устройство целиком в
// kmalloc'нутый буфер, а unmount синхронизирует "грязные" секторы обратно.
// fat_flush()/fat_sync() из fat.c не используются — они жёстко пишут на
// ATA-диск (саму LufiraFS), запись назад реализована здесь отдельно через
// xhci_msd_write_block().
//
// Именованные монтирования (mount <usb> [name]), а не настоящие пути VFS:
// LufiraFS-инод не может указывать на FAT-запись (это две независимые
// системы адресации), так что "cd в примонтированную флешку" тут
// НЕ поддерживается — доступ к файлам внутри конкретного монтирования идёт
// через mountls/mountcat/mountwrite <name> <...>, ограниченные КОРНЕВЫМ
// каталогом каждого монтирования (тот же предел, что и у первоначального
// листинга при mount — полноценный путь-резолвер типа fat_lookup_path()
// сюда сознательно не выносится, это отдельная, более крупная задача).
#include "../commands.h"
#include "drivers/console/console.h"
#include "drivers/usb/xhci.h"
#include "fs/fat/fat.h"
#include "system/mm/heap.h"
#include "lib/string.h"

// Каждый маунт — свой fat_fs_t + свой image-буфер в куче. 2, а не больше:
// каждый образ до MOUNT_MAX_IMAGE_BYTES целиком живёт в 16MB куче ядра
// (см. heap.h) вместе со всем остальным, что там когда-либо kmalloc'ается —
// больше двух многомегабайтных образов одновременно рискует её исчерпать.
#define MAX_MOUNTS 2
#define MOUNT_NAME_MAX 16

typedef struct {
    int in_use;
    char name[MOUNT_NAME_MAX];
    int usb_index;
    fat_fs_t fs;
    uint8_t *image_buf;
} fat_mount_t;

static fat_mount_t g_mounts[MAX_MOUNTS];

// Как copy_bounded() в users.c — своего strncpy в этом freestanding
// lib/string.h нет. max_len тут ЁМКОСТЬ БУФЕРА (out), не длина src; режет
// src ещё и по первому пробелу (имена монтирований — один токен).
static void copy_bounded_name(char *out, const char *src, int max_len) {
    int i = 0;
    while (i < max_len - 1 && src[i] && src[i] != ' ' && src[i] != '\t') { out[i] = src[i]; i++; }
    out[i] = '\0';
}

// Верхняя граница образа ФС, который можно целиком держать в RAM.
#define MOUNT_MAX_IMAGE_BYTES (8u * 1024u * 1024u)

// Изредка под нагрузкой (сотни вызовов подряд) xhci_msd_read/write_block
// может не уложиться в таймаут или вернуть ошибку контроллера — небольшой
// retry остаётся дешёвой защитой на случай единичного сбоя.
#define MOUNT_IO_RETRIES 3

static int read_blocks_retry(int usb_index, uint32_t lba, uint32_t count, void *buf, uint32_t block_size) {
    for (int attempt = 0; attempt < MOUNT_IO_RETRIES; attempt++) {
        if (xhci_msd_read_blocks(usb_index, lba, count, buf, block_size) == 0) return 0;
    }
    return -1;
}

static int write_blocks_retry(int usb_index, uint32_t lba, uint32_t count, const void *buf, uint32_t block_size) {
    for (int attempt = 0; attempt < MOUNT_IO_RETRIES; attempt++) {
        if (xhci_msd_write_blocks(usb_index, lba, count, buf, block_size) == 0) return 0;
    }
    return -1;
}

static fat_mount_t *find_mount_by_name(const char *name) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (g_mounts[i].in_use && strcmp(g_mounts[i].name, name) == 0) return &g_mounts[i];
    }
    return NULL;
}

static int count_active_mounts(void) {
    int n = 0;
    for (int i = 0; i < MAX_MOUNTS; i++) if (g_mounts[i].in_use) n++;
    return n;
}

static int mount_fat_from_usb(int usb_index, const char *name) {
    if (find_mount_by_name(name)) {
        printf("\nmount: name '%s' already in use\n", name);
        return -1;
    }

    int slot = -1;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!g_mounts[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        printf("\nmount: too many mounts (max %d) - unmount one first\n", MAX_MOUNTS);
        return -1;
    }

    uint32_t max_lba, block_size;
    if (xhci_msd_get_info(usb_index, &max_lba, &block_size) != 0) {
        printf("\nmount: no such device: usb%d\n", usb_index);
        return -1;
    }
    if (block_size != 512) {
        printf("\nmount: unsupported block size %u (FAT driver only supports 512)\n", block_size);
        return -1;
    }

    uint64_t total_bytes = ((uint64_t)max_lba + 1) * block_size;
    if (total_bytes > MOUNT_MAX_IMAGE_BYTES) {
        printf("\nmount: device too large (%lu KB, max %u KB) - FAT driver keeps the whole image in RAM\n",
               (unsigned long)(total_bytes / 1024), MOUNT_MAX_IMAGE_BYTES / 1024);
        return -1;
    }

    uint8_t *buf = (uint8_t *)kmalloc((size_t)total_bytes);
    if (!buf) {
        printf("\nmount: not enough memory (%lu KB)\n", (unsigned long)(total_bytes / 1024));
        return -1;
    }

    uint32_t total_blocks = max_lba + 1;
    printf("\nReading usb%d (%u blocks)...\n", usb_index, total_blocks);
    for (uint32_t lba = 0; lba < total_blocks; ) {
        uint32_t remaining = total_blocks - lba;
        uint32_t batch = remaining > XHCI_MSD_MAX_BATCH_BLOCKS ? XHCI_MSD_MAX_BATCH_BLOCKS : remaining;
        if (read_blocks_retry(usb_index, lba, batch, buf + (uint64_t)lba * block_size, block_size) != 0) {
            printf("mount: read failed at LBA %u (after %d retries)\n", lba, MOUNT_IO_RETRIES);
            kfree(buf);
            return -1;
        }
        lba += batch;
    }

    fat_mount_t *m = &g_mounts[slot];
    int res = fat_init(&m->fs, buf, (uint32_t)total_bytes);
    if (res != 0) {
        printf("mount: not a FAT filesystem (fat_init error %d)\n", res);
        kfree(buf);
        return -1;
    }

    m->image_buf = buf;
    m->usb_index = usb_index;
    copy_bounded_name(m->name, name, sizeof(m->name));
    m->in_use = 1;
    return slot;
}

// Записывает "грязные" секторы обратно на флешку. Не переиспользует
// fat_flush()/fat_sync() (fat.c) — те жёстко пишут через
// disk_write_sectors() (ATA-диск с самой LufiraFS), что при вызове отсюда
// молча испортило бы disk.img вместо флешки.
static void mount_fat_sync_to_usb(fat_mount_t *m) {
    uint32_t written = 0, failed = 0;
    uint32_t total = m->fs.total_sectors;
    uint32_t lba = 0;
    // Батчит СОСЕДНИЕ "грязные" секторы в один write_blocks_retry() вместо
    // одного вызова на бит — dirty_map почти всегда содержит длинные
    // прогоны подряд идущих секторов (директории/FAT-таблица растут
    // последовательно), так что почти всё синкается по 64KB за раз.
    while (lba < total) {
        if (!(m->fs.dirty_map[lba >> 3] & (1 << (lba & 7)))) { lba++; continue; }

        uint32_t run_start = lba;
        uint32_t run_len = 0;
        while (lba < total && run_len < XHCI_MSD_MAX_BATCH_BLOCKS &&
               (m->fs.dirty_map[lba >> 3] & (1 << (lba & 7)))) {
            run_len++;
            lba++;
        }

        if (write_blocks_retry(m->usb_index, run_start, run_len,
                                m->fs.image + (uint64_t)run_start * 512, 512) == 0) {
            written += run_len;
        } else {
            failed += run_len;
        }
    }
    memset(m->fs.dirty_map, 0, m->fs.dirty_map_size);

    printf("\n[MOUNT] Synced %u sector(s) to usb%d ('%s')", written, m->usb_index, m->name);
    if (failed > 0) printf(" (%u FAILED - flash card may be inconsistent, re-mount to check)", failed);
    printf("\n");
}

static void unmount_fat(fat_mount_t *m) {
    mount_fat_sync_to_usb(m);
    kfree(m->fs.dirty_map);
    kfree(m->image_buf);
    m->image_buf = NULL;
    m->in_use = 0;
}

// 8.3 короткое имя из fat_dir_entry_t.name (11 байт, без разделителя) в
// привычный человеку вид "NAME.EXT".
static void format_short_name(const uint8_t raw[11], char out[13]) {
    int pos = 0;
    for (int j = 0; j < 8 && raw[j] != ' '; j++) out[pos++] = (char)raw[j];
    if (raw[8] != ' ') {
        out[pos++] = '.';
        for (int j = 8; j < 11 && raw[j] != ' '; j++) out[pos++] = (char)raw[j];
    }
    out[pos] = '\0';
}

static void list_mount_root(fat_mount_t *m) {
    fat_dir_t dir;
    if (fat_opendir(&m->fs, m->fs.root_cluster, &dir) != 0) {
        printf("(failed to open root directory)\n");
        return;
    }
    fat_dir_entry_t entry;
    char name[13];
    int count = 0;
    printf("\n");
    while (fat_readdir(&dir, &entry) == 1) {
        format_short_name(entry.name, name);
        printf("%s  ", name);
        if (++count % 4 == 0) printf("\n");
    }
    fat_closedir(&dir);
    if (count % 4 != 0) printf("\n");
    if (count == 0) printf("(empty)\n");
}

// mount [<usb-index> [name]] — без аргументов перечисляет активные
// монтирования; с индексом монтирует новое (имя по умолчанию "mnt<индекс>").
void command_mount(const char *args) {
    if (!args || !*args) {
        int n = count_active_mounts();
        if (n == 0) {
            printf("\nNo active mounts. Usage: mount <usb-device-index> [name]\n");
            printf("Example: mount 0  (see 'usbinfo' for the list of devices)\n");
            return;
        }
        printf("\nActive mounts:\n");
        for (int i = 0; i < MAX_MOUNTS; i++) {
            if (!g_mounts[i].in_use) continue;
            fat_mount_t *m = &g_mounts[i];
            printf(" %s: usb%d, FAT%d, %u total sectors\n",
                   m->name, m->usb_index, m->fs.fat_type, m->fs.total_sectors);
        }
        return;
    }

    const char *p = skip_spaces(args);
    int idx_len = token_length(p);
    char idx_buf[8];
    int cl = idx_len < (int)sizeof(idx_buf) - 1 ? idx_len : (int)sizeof(idx_buf) - 1;
    for (int i = 0; i < cl; i++) idx_buf[i] = p[i];
    idx_buf[cl] = '\0';
    int usb_index = atoi(idx_buf);

    char name[MOUNT_NAME_MAX];
    const char *rest = skip_spaces(p + idx_len);
    if (*rest) {
        copy_bounded_name(name, rest, sizeof(name));
    } else {
        // Имя по умолчанию "mnt<индекс>".
        const char *prefix = "mnt";
        int pos = 0;
        while (prefix[pos] && pos < (int)sizeof(name) - 1) { name[pos] = prefix[pos]; pos++; }
        char idx_dec[8];
        int dp = 0;
        if (usb_index == 0) idx_dec[dp++] = '0';
        else {
            int tmp = usb_index, digits[8], nd = 0;
            while (tmp > 0 && nd < 8) { digits[nd++] = tmp % 10; tmp /= 10; }
            while (nd > 0) idx_dec[dp++] = (char)('0' + digits[--nd]);
        }
        idx_dec[dp] = '\0';
        int di = 0;
        while (idx_dec[di] && pos < (int)sizeof(name) - 1) name[pos++] = idx_dec[di++];
        name[pos] = '\0';
    }

    int slot = mount_fat_from_usb(usb_index, name);
    if (slot < 0) return;

    fat_mount_t *m = &g_mounts[slot];
    printf("Mounted usb%d as '%s': FAT%d, cluster=%u bytes, %u total sectors, root cluster=%u\n",
           m->usb_index, m->name, m->fs.fat_type, m->fs.cluster_size * 512, m->fs.total_sectors, m->fs.root_cluster);

    // Листинг корня — наглядное подтверждение, что драйвер реально читает
    // структуру ФС, а не просто принял образ.
    list_mount_root(m);
}

// unmount [name] — без аргумента unmount'ит ЕДИНСТВЕННОЕ активное
// монтирование (если их несколько или ни одного — требует явного имени).
void command_unmount(const char *args) {
    fat_mount_t *m = NULL;

    if (!args || !*args) {
        if (count_active_mounts() == 1) {
            for (int i = 0; i < MAX_MOUNTS; i++) if (g_mounts[i].in_use) { m = &g_mounts[i]; break; }
        } else {
            printf("\numount: %s - specify a name (see 'mount')\n",
                   count_active_mounts() == 0 ? "nothing mounted" : "multiple mounts active");
            return;
        }
    } else {
        char name[MOUNT_NAME_MAX];
        copy_bounded_name(name, skip_spaces(args), sizeof(name));
        m = find_mount_by_name(name);
        if (!m) {
            printf("\numount: no such mount: %s\n", name);
            return;
        }
    }

    char name_copy[MOUNT_NAME_MAX];
    copy_bounded_name(name_copy, m->name, sizeof(name_copy));
    int idx = m->usb_index;
    unmount_fat(m);
    printf("\nUnmounted usb%d ('%s')\n", idx, name_copy);
}

// mountls <name> — листинг корневого каталога уже смонтированной ФС (тот
// же вывод, что печатается один раз при mount, но по требованию повторно).
void command_mountls(const char *args) {
    if (!args || !*args) {
        printf("\nUsage: mountls <name>\n");
        return;
    }
    char name[MOUNT_NAME_MAX];
    copy_bounded_name(name, skip_spaces(args), sizeof(name));
    fat_mount_t *m = find_mount_by_name(name);
    if (!m) {
        printf("\nmountls: no such mount: %s\n", name);
        return;
    }
    list_mount_root(m);
}

// mountcat <name> <file> — читает файл из КОРНЯ монтирования <name> и
// печатает его содержимое (как cat, но в сторону от LufiraFS/VFS).
void command_mountcat(const char *args) {
    if (!args || !*args) {
        printf("\nUsage: mountcat <name> <file>\n");
        return;
    }
    char name[MOUNT_NAME_MAX];
    const char *p = skip_spaces(args);
    int nlen = token_length(p);
    int cl = nlen < (int)sizeof(name) - 1 ? nlen : (int)sizeof(name) - 1;
    for (int i = 0; i < cl; i++) name[i] = p[i];
    name[cl] = '\0';

    const char *filename = skip_spaces(p + nlen);
    if (!*filename) {
        printf("\nUsage: mountcat <name> <file>\n");
        return;
    }

    fat_mount_t *m = find_mount_by_name(name);
    if (!m) {
        printf("\nmountcat: no such mount: %s\n", name);
        return;
    }

    uint32_t size;
    if (fat_open(&m->fs, filename, &size) != 0) {
        printf("\nmountcat: file not found: %s\n", filename);
        return;
    }

    uint8_t *buf = (uint8_t *)kmalloc(size > 0 ? size : 1);
    if (!buf) {
        printf("\nmountcat: not enough memory\n");
        return;
    }
    if (fat_read_file(&m->fs, filename, buf, size) < 0) {
        printf("\nmountcat: error reading file\n");
        kfree(buf);
        return;
    }

    printf("\n");
    for (uint32_t i = 0; i < size; i++) put_char((char)buf[i]);
    printf("\n");
    kfree(buf);
}

// mountwrite <name> <file> <text> — создаёт/перезаписывает файл в корне
// монтирования <name>. Как write/usbwrite — вызывается с СЫРЫМ
// input_buffer (см. shell.c), текст регистрозависим.
void command_mountwrite(const char *args) {
    if (!args || !*args) {
        printf("\nUsage: mountwrite <name> <file> <text>\n");
        return;
    }
    char name[MOUNT_NAME_MAX], filename[64];
    const char *p = skip_spaces(args);
    int nlen = token_length(p);
    int cl = nlen < (int)sizeof(name) - 1 ? nlen : (int)sizeof(name) - 1;
    for (int i = 0; i < cl; i++) name[i] = p[i];
    name[cl] = '\0';

    p = skip_spaces(p + nlen);
    int flen = token_length(p);
    cl = flen < (int)sizeof(filename) - 1 ? flen : (int)sizeof(filename) - 1;
    for (int i = 0; i < cl; i++) filename[i] = p[i];
    filename[cl] = '\0';

    const char *text = skip_spaces(p + flen);
    if (filename[0] == '\0' || !*text) {
        printf("\nUsage: mountwrite <name> <file> <text>\n");
        return;
    }

    fat_mount_t *m = find_mount_by_name(name);
    if (!m) {
        printf("\nmountwrite: no such mount: %s\n", name);
        return;
    }

    uint32_t size;
    if (fat_open(&m->fs, filename, &size) != 0) {
        if (fat_create_file(&m->fs, m->fs.root_cluster, filename) != 0) {
            printf("\nmountwrite: failed to create file\n");
            return;
        }
    }

    int text_len = 0;
    while (text[text_len]) text_len++;

    if (fat_write_file(&m->fs, filename, text, (uint32_t)text_len) < 0) {
        printf("\nmountwrite: write failed\n");
        return;
    }

    printf("\nWrote %d bytes to '%s' on mount '%s'\n", text_len, filename, name);
}
