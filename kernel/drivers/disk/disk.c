#include "disk.h"
#include "ahci.h"
#include "nvme.h"
#include "lib/types.h"
#include "drivers/console/console.h"
#include "system/devmode/devmode.h"
#include "system/timer/pit.h"
#include "lib/cpu.h"

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

// Легаси ATA PIO — запасной путь, если AHCI/NVMe не найдены. В этом случае
// на портах 0x1F0-0x1F7 физически нет устройства (диск в QEMU подключён
// только через выбранный --disk-mode контроллер), поэтому ожидания должны
// укладываться в предсказуемый таймаут по реальному времени, а не по числу
// итераций. ata_wait_ready()/ata_wait_drq() — тот же гибрид, что в
// ahci.c/nvme.c: короткий чистый спин (реальное устройство отвечает за
// микросекунды), затем pit_wait_ms() с лимитом по времени.
//
// pit_wait_ms() ждёт прерывания (hlt) и провисит вечно, если прерывания ещё
// не включены — а klog() пишет на LufiraFS уже до disk_init()/sti в
// kernel.c. cpu_interrupts_active() (lib/cpu.c) проверяет это: до sti
// медленный путь пропускается целиком, остаётся только быстрый спин.
#define ATA_WAIT_FAST_SPIN_ITERS 20000
#define ATA_WAIT_SLOW_TIMEOUT_MS 500

static int ata_wait_ready(void) {
    for (volatile int i = 0; i < ATA_WAIT_FAST_SPIN_ITERS; i++) {
        uint8_t status = inb(ATA_STATUS);
        if (!(status & 0x80)) return 0;   // BSY clear -> ready
    }
    if (!cpu_interrupts_active()) return -1;
    for (int i = 0; i < ATA_WAIT_SLOW_TIMEOUT_MS; i++) {
        pit_wait_ms(1);
        uint8_t status = inb(ATA_STATUS);
        if (!(status & 0x80)) return 0;
    }
    return -1;
}

static int ata_wait_drq(void) {
    for (volatile int i = 0; i < ATA_WAIT_FAST_SPIN_ITERS; i++) {
        uint8_t status = inb(ATA_STATUS);
        if (status & 0x08) return 0;      // DRQ set
        if (status & 0x01) return -2;     // error
    }
    if (!cpu_interrupts_active()) return -1;
    for (int i = 0; i < ATA_WAIT_SLOW_TIMEOUT_MS; i++) {
        pit_wait_ms(1);
        uint8_t status = inb(ATA_STATUS);
        if (status & 0x08) return 0;
        if (status & 0x01) return -2;
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

// Наличие устройства на легаси ATA-портах кэшируется один раз при
// disk_init(): иначе каждый disk_write_sectors() без реального диска
// (lufirafs_sync() зовёт его по всему ядру, в т.ч. из klog()) платил бы
// полным таймаутом ata_wait_ready()/ata_wait_drq() за каждый грязный блок.
//
// Проверка "плавающая шина" (STATUS==0xFF) не годится: QEMU поднимает сам
// IDE-контроллер (PIIX) безусловно, даже без -drive if=ide, так что STATUS
// не 0xFF, хотя диска нет. Надёжный тест (OSDev "ATA PIO Mode"): записать
// паттерн в LBA_MID/LBA_HIGH и прочитать обратно — эти регистры защёлкивает
// только реально существующее устройство.
static int g_ata_present = 0;

static void ata_detect(void) {
    outb(ATA_DRIVE_SEL, 0xA0); // master, LBA-биты не нужны для самого теста
    outb(ATA_LBA_MID, 0x5A);
    outb(ATA_LBA_HIGH, 0xA5);
    g_ata_present = (inb(ATA_LBA_MID) == 0x5A && inb(ATA_LBA_HIGH) == 0xA5);
}

void disk_init(void) {
    if (ahci_init() == 0) {
        DLOG("[DISK] Using AHCI\n");
    } else if (nvme_init() == 0) {
        DLOG("[DISK] Using NVMe\n");
    } else {
        ata_detect();
        DLOG("[DISK] No AHCI/NVMe controller found — falling back to legacy ATA PIO (device %s)\n",
             g_ata_present ? "present" : "absent");
    }
}

int disk_read_sectors(uint32_t lba, uint8_t sector_count, void *buffer) {
    if (ahci_available())
        return ahci_read_sectors(lba, sector_count, buffer);
    if (nvme_available())
        return nvme_read_sectors(lba, sector_count, buffer);
    if (!g_ata_present) return -1;

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
    if (ahci_available())
        return ahci_write_sectors(lba, sector_count, buffer);
    if (nvme_available())
        return nvme_write_sectors(lba, sector_count, buffer);
    if (!g_ata_present) return -1;

    const uint16_t *buf = (const uint16_t*)buffer;
    for (uint8_t s = 0; s < sector_count; s++) {
        if (ata_wait_ready() != 0) return -1;
        ata_select_drive_lba(lba + s);
        outb(ATA_COMMAND, ATA_CMD_WRITE);
        if (ata_wait_drq() != 0) return -1;
        for (int i = 0; i < 256; i++)
            outw(ATA_DATA, buf[i + s * 256]);
        // Дожидаемся сброса BSY после записи (тот же тест, что в ata_wait_ready()).
        ata_wait_ready();
    }
    return 0;
}