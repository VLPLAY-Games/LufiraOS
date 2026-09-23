// Команды USB Mass Storage — только блочное чтение/запись (без
// монтирования файловой системы, см. план: VFS сегодня не поддерживает
// вторую ФС вообще). Драйвер — kernel/drivers/usb/xhci.c.
#include "../commands.h"
#include "drivers/console/console.h"
#include "drivers/usb/xhci.h"
#include "lib/string.h"

// usbinfo — список найденных USB-накопителей (ёмкость, размер блока).
void command_usbinfo(void) {
    int count = xhci_msd_device_count();
    if (count == 0) {
        printf("\nNo USB mass storage devices found\n");
        return;
    }

    printf("\n");
    for (int i = 0; i < count; i++) {
        uint32_t max_lba, block_size;
        if (xhci_msd_get_info(i, &max_lba, &block_size) != 0) continue;

        uint64_t total_bytes = ((uint64_t)max_lba + 1) * block_size;
        uint64_t total_kb = total_bytes / 1024;
        printf("usb%d: %u blocks x %u bytes = %lu KB\n", i, max_lba + 1, block_size, total_kb);
    }
}

// usbread <index> <lba> — читает один блок и печатает первые байты как hex
// и ASCII (насколько влезает в строку), чтобы наглядно подтвердить чтение.
void command_usbread(const char *args) {
    if (!args || !*args) {
        printf("\nUsage: usbread <device> <lba>\n");
        return;
    }

    int index = atoi(args);
    const char *p = args;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    if (*p == '\0') {
        printf("\nUsage: usbread <device> <lba>\n");
        return;
    }
    uint32_t lba = (uint32_t)atoi(p);

    uint32_t max_lba, block_size;
    if (xhci_msd_get_info(index, &max_lba, &block_size) != 0) {
        printf("\nusbread: no such device: usb%d\n", index);
        return;
    }
    if (lba > max_lba) {
        printf("\nusbread: LBA %u out of range (max %u)\n", lba, max_lba);
        return;
    }

    uint8_t buf[512];
    if (block_size > sizeof(buf)) {
        printf("\nusbread: block size %u too large\n", block_size);
        return;
    }

    if (xhci_msd_read_block(index, lba, buf, block_size) != 0) {
        printf("\nusbread: read failed\n");
        return;
    }

    printf("\n--- usb%d LBA %u (%u bytes) ---\n", index, lba, block_size);
    uint32_t show = block_size < 64 ? block_size : 64;
    for (uint32_t i = 0; i < show; i++) {
        printf("%x ", buf[i]);
        if ((i % 16) == 15) printf("\n");
    }
    printf("\n--- end ---\n");
}

// usbwrite <index> <lba> <text> — пишет text (с завершающим нулём) в
// начало блока, остаток блока обнуляется. Для наглядной проверки
// round-trip'а (usbwrite ... hello; usbread ...).
void command_usbwrite(const char *args) {
    if (!args || !*args) {
        printf("\nUsage: usbwrite <device> <lba> <text>\n");
        return;
    }

    int index = atoi(args);
    const char *p = args;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    if (*p == '\0') {
        printf("\nUsage: usbwrite <device> <lba> <text>\n");
        return;
    }
    uint32_t lba = (uint32_t)atoi(p);

    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    if (*p == '\0') {
        printf("\nUsage: usbwrite <device> <lba> <text>\n");
        return;
    }
    const char *text = p;

    uint32_t max_lba, block_size;
    if (xhci_msd_get_info(index, &max_lba, &block_size) != 0) {
        printf("\nusbwrite: no such device: usb%d\n", index);
        return;
    }
    if (lba > max_lba) {
        printf("\nusbwrite: LBA %u out of range (max %u)\n", lba, max_lba);
        return;
    }

    uint8_t buf[512];
    if (block_size > sizeof(buf)) {
        printf("\nusbwrite: block size %u too large\n", block_size);
        return;
    }
    memset(buf, 0, block_size);

    int len = 0;
    while (text[len] && (uint32_t)len < block_size) len++;
    memcpy(buf, text, (size_t)len);

    if (xhci_msd_write_block(index, lba, buf, block_size) != 0) {
        printf("\nusbwrite: write failed\n");
        return;
    }

    printf("\nusb%d: wrote %d bytes to LBA %u\n", index, len, lba);
}
