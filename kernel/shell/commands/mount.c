// Команды mount/unmount — активируют FAT-драйвер (fs/fat/fat.c) для USB-
// флешек. VFS сегодня не умеет монтировать вторую ФС (нет реестра
// filesystem_t, всё жёстко завязано на LufiraFS), поэтому это не настоящий
// VFS-mount, а отдельная подсистема поверх одного глобального fat_fs_t,
// работающая напрямую с xhci_msd_*/fat_*, в стороне от VFS — как usbinfo/
// usbread/usbwrite (usb.c). fat_init() ожидает образ ФС целиком в RAM, так
// что mount читает устройство целиком в kmalloc'нутый буфер, а unmount
// синхронизирует "грязные" секторы обратно. fat_flush()/fat_sync() из fat.c
// не используются — они жёстко пишут на ATA-диск (саму LufiraFS), запись
// назад реализована здесь отдельно через xhci_msd_write_block().
#include "../commands.h"
#include "drivers/console/console.h"
#include "drivers/usb/xhci.h"
#include "fs/fat/fat.h"
#include "system/mm/heap.h"
#include "lib/string.h"

// Единственная точка монтирования (v1). mount_fat_from_usb()/
// mount_fat_sync_to_usb()/unmount_fat() не завязаны на shell.c и принимают
// только int usb_index — годятся для выноса в отдельный пакет в v0.7 как есть.
static fat_fs_t fatfs;
static int fat_mounted = 0;
static int fat_usb_index = -1;
static uint8_t *fat_image_buf = NULL;

// Верхняя граница образа ФС, который можно целиком держать в RAM. Куча
// ядра — 16 MB суммарно на всё (KERNEL_HEAP_SIZE, kernel/system/mm/heap.h),
// делится со всем остальным, что когда-либо kmalloc'ается — 8 MB оставляет
// разумный запас.
#define MOUNT_MAX_IMAGE_BYTES (8u * 1024u * 1024u)

// Изредка под нагрузкой (сотни вызовов подряд) xhci_msd_read/write_block
// может не уложиться в таймаут или вернуть ошибку контроллера — небольшой
// retry остаётся дешёвой защитой на случай единичного сбоя.
#define MOUNT_IO_RETRIES 3

static int read_block_retry(int usb_index, uint32_t lba, void *buf, uint32_t block_size) {
    for (int attempt = 0; attempt < MOUNT_IO_RETRIES; attempt++) {
        if (xhci_msd_read_block(usb_index, lba, buf, block_size) == 0) return 0;
    }
    return -1;
}

static int write_block_retry(int usb_index, uint32_t lba, const void *buf, uint32_t block_size) {
    for (int attempt = 0; attempt < MOUNT_IO_RETRIES; attempt++) {
        if (xhci_msd_write_block(usb_index, lba, buf, block_size) == 0) return 0;
    }
    return -1;
}

static int mount_fat_from_usb(int usb_index) {
    if (fat_mounted) {
        printf("\nmount: already have a filesystem mounted (usb%d) - unmount first\n", fat_usb_index);
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
    for (uint32_t lba = 0; lba < total_blocks; lba++) {
        if (read_block_retry(usb_index, lba, buf + (uint64_t)lba * block_size, block_size) != 0) {
            printf("mount: read failed at LBA %u (after %d retries)\n", lba, MOUNT_IO_RETRIES);
            kfree(buf);
            return -1;
        }
    }

    int res = fat_init(&fatfs, buf, (uint32_t)total_bytes);
    if (res != 0) {
        printf("mount: not a FAT filesystem (fat_init error %d)\n", res);
        kfree(buf);
        return -1;
    }

    fat_image_buf = buf;
    fat_usb_index = usb_index;
    fat_mounted = 1;
    return 0;
}

// Записывает "грязные" секторы обратно на флешку. Не переиспользует
// fat_flush()/fat_sync() (fat.c) — те жёстко пишут через
// disk_write_sectors() (ATA-диск с самой LufiraFS), что при вызове отсюда
// молча испортило бы disk.img вместо флешки.
static void mount_fat_sync_to_usb(void) {
    uint32_t written = 0, failed = 0;
    for (uint32_t lba = 0; lba < fatfs.total_sectors; lba++) {
        if (!(fatfs.dirty_map[lba >> 3] & (1 << (lba & 7)))) continue;
        if (write_block_retry(fat_usb_index, lba, fatfs.image + (uint64_t)lba * 512, 512) == 0) {
            written++;
        } else {
            failed++;
        }
    }
    memset(fatfs.dirty_map, 0, fatfs.dirty_map_size);

    printf("\n[MOUNT] Synced %u sector(s) to usb%d", written, fat_usb_index);
    if (failed > 0) printf(" (%u FAILED - flash card may be inconsistent, re-mount to check)", failed);
    printf("\n");
}

static void unmount_fat(void) {
    if (!fat_mounted) return;
    mount_fat_sync_to_usb();
    kfree(fatfs.dirty_map);
    kfree(fat_image_buf);
    fat_image_buf = NULL;
    fat_mounted = 0;
    fat_usb_index = -1;
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

void command_mount(const char *args) {
    if (!args || !*args) {
        printf("\nUsage: mount <usb-device-index>\n");
        printf("Example: mount 0  (see 'usbinfo' for the list of devices)\n");
        return;
    }

    int usb_index = atoi(args);
    if (mount_fat_from_usb(usb_index) != 0) return;

    printf("Mounted usb%d: FAT%d, cluster=%u bytes, %u total sectors, root cluster=%u\n",
           fat_usb_index, fatfs.fat_type, fatfs.cluster_size * 512, fatfs.total_sectors, fatfs.root_cluster);

    // Листинг корня — наглядное подтверждение, что драйвер реально читает
    // структуру ФС, а не просто принял образ.
    fat_dir_t dir;
    if (fat_opendir(&fatfs, fatfs.root_cluster, &dir) != 0) {
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

void command_unmount(void) {
    if (!fat_mounted) {
        printf("\numount: nothing mounted\n");
        return;
    }
    int idx = fat_usb_index;
    unmount_fat();
    printf("\nUnmounted usb%d\n", idx);
}
