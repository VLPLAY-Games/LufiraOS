#include "disk.h"
#include <stdint.h>

#define ATA_PRIMARY_IO  0x1F0
#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECTOR_COUNT 0x1F2
#define ATA_LBA_LOW     0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HIGH    0x1F5
#define ATA_DRIVE_SEL   0x1F6
#define ATA_COMMAND     0x1F7
#define ATA_STATUS      0x1F7

#define ATA_CMD_READ    0x20
#define ATA_CMD_WRITE   0x30

static inline uint8_t inb(uint16_t port) {
    uint8_t data;
    asm volatile("inb %1, %0" : "=a"(data) : "Nd"(port));
    return data;
}
static inline void outb(uint16_t port, uint8_t data) {
    asm volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t data;
    asm volatile("inw %1, %0" : "=a"(data) : "Nd"(port));
    return data;
}
static inline void outw(uint16_t port, uint16_t data) {
    asm volatile("outw %0, %1" : : "a"(data), "Nd"(port));
}

static int ata_wait_ready(void) {
    for (volatile int i = 0; i < 100000; i++) {
        uint8_t status = inb(ATA_STATUS);
        if (!(status & 0x80)) return 0;   // BSY clear -> ready
        for (volatile int j = 0; j < 1000; j++);
    }
    return -1;
}

static int ata_wait_drq(void) {
    for (volatile int i = 0; i < 100000; i++) {
        uint8_t status = inb(ATA_STATUS);
        if (status & 0x08) return 0;      // DRQ set
        if (status & 0x01) return -2;     // error
        for (volatile int j = 0; j < 1000; j++);
    }
    return -1;
}

static void ata_select_drive_lba(uint32_t lba) {
    outb(ATA_DRIVE_SEL, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECTOR_COUNT, 1);
    outb(ATA_LBA_LOW, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
}

int disk_read_sectors(uint32_t lba, uint8_t sector_count, void *buffer) {
    uint16_t *buf = (uint16_t*)buffer;
    for (uint8_t s = 0; s < sector_count; s++) {
        if (ata_wait_ready() != 0) return -1;
        ata_select_drive_lba(lba + s);
        outb(ATA_COMMAND, ATA_CMD_READ);
        if (ata_wait_drq() != 0) return -1;
        for (int i = 0; i < 256; i++)
            buf[i + s * 256] = inw(ATA_DATA);
    }
    return 0;
}

int disk_write_sectors(uint32_t lba, uint8_t sector_count, const void *buffer) {
    const uint16_t *buf = (const uint16_t*)buffer;
    for (uint8_t s = 0; s < sector_count; s++) {
        if (ata_wait_ready() != 0) return -1;
        ata_select_drive_lba(lba + s);
        outb(ATA_COMMAND, ATA_CMD_WRITE);
        if (ata_wait_drq() != 0) return -1;
        for (int i = 0; i < 256; i++)
            outw(ATA_DATA, buf[i + s * 256]);
        // Дожидаемся сброса BSY после записи
        for (volatile int j = 0; j < 100000; j++) {
            uint8_t status = inb(ATA_STATUS);
            if (!(status & 0x80)) break;
            for (volatile int k = 0; k < 1000; k++);
        }
    }
    return 0;
}