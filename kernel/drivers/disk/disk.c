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

// НАЙДЕННЫЙ БАГ (пользователь: AHCI/NVMe-режимы грузятся в 10-15 раз
// медленнее IDE, с фризами по 10-20 секунд СРАЗУ ПОСЛЕ попытки найти
// AHCI/NVMe-контроллер) — настоящая причина была именно здесь, не в самих
// ahci.c/nvme.c. disk_init() (disk.c) при неудаче AHCI/NVMe молча
// откатывается на этот legacy ATA PIO путь — но диск в QEMU подключён
// ТОЛЬКО через выбранный --disk-mode контроллер, так что если выбран
// ahci/nvme и его инициализация не удалась, на легаси ATA-портах
// (0x1F0-0x1F7) физически НЕТ устройства. ata_wait_ready()/ata_wait_drq()
// раньше были ВЛОЖЕННЫМ busy-wait'ом без единого pit_wait_ms() — внешний
// цикл 100000 итераций, внутри КАЖДОЙ — ещё 1000 пустых итераций — до
// 100 000 000 холостых проходов на один таймаут, и это на КАЖДЫЙ сектор
// (disk_read/write_sectors() вызывают его в цикле по sector_count), и
// disk_write_sectors() ждёт так же ещё и после записи (см. ниже). Первая
// же синхронизация LufiraFS на диск без устройства — и это кратно
// умножается. В IDE-режиме диск реально есть на этих портах, поэтому
// быстрый путь (первая же проверка status) срабатывает сразу — отсюда и
// разница "IDE быстро, AHCI/NVMe фриз на 10-20с", никак не связанная с
// самими AHCI/NVMe контроллерами. Тот же гибрид, что уже применён в
// ahci.c/nvme.c: короткий чистый спин (реальное устройство отвечает за
// микросекунды, без добавленной задержки), затем pit_wait_ms(), с
// ограничением по РЕАЛЬНОМУ времени вместо голого числа итераций — так
// отсутствующее устройство обнаруживается за разумный, предсказуемый
// таймаут, а не зависит от скорости хоста/эмуляции.
//
// НАЙДЕННЫЙ БАГ №2 (живым тестом, при первой же попытке): в отличие от
// ahci.c/nvme.c (где pit_wait_ms() в быстром/медленном гибриде достижим
// ТОЛЬКО через *_available()==1, что физически невозможно раньше
// disk_init()/sti), disk_read_sectors()/disk_write_sectors() — общая точка
// входа БЕЗ такой гарантии: klog() уже пишет на LufiraFS (klog_init()
// в kernel.c) ЗАДОЛГО до disk_init()/sti — то есть ata_wait_ready()/
// ata_wait_drq() могут быть вызваны, когда прерывания ещё выключены.
// pit_wait_ms() в этот момент виснет НАВСЕГДА (hlt никогда не проснётся —
// тот же класс бага, что уже чинили для hda_init()/xhci_init(), см. их
// комментарии). cpu_interrupts_active() (lib/cpu.c, выставляется в
// kernel.c сразу после sti) — явная проверка на этот случай: если
// прерывания ещё не разрешены, медленный pit_wait_ms()-путь пропускаем
// целиком и остаёмся на уже пройденном быстром спине (как было ДО этого
// фикса для самого раннего пути — отсутствующее устройство в эту секунду
// загрузки штатно и ожидаемо, долгий таймаут здесь не нужен).
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

// НАЙДЕННЫЙ БАГ №3 (живым тестом — зависание на МИНУТЫ сразу после
// "[PROCESS] Created 'shell'..."): lufirafs_sync() (см. lufirafs.c) снимает
// "грязный" бит блока ТОЛЬКО при успешном disk_write_sectors() — если
// устройства на легаси ATA-портах вообще нет (типичный случай, когда
// выбранный --disk-mode ahci/nvme не нашёл контроллер — диск в QEMU
// подключён ТОЛЬКО через него), ни один блок никогда не очистится, и
// каждый следующий вызов lufirafs_sync() (а их ~25 точек по всему ядру,
// включая klog() — он зовётся уже из process_create() на каждый новый
// процесс) заново перебирает ВСЕ накопленные с начала загрузки грязные
// блоки, каждый из которых стоит до ATA_WAIT_SLOW_TIMEOUT_MS*2 (ready+drq)
// реального времени — набегающий, НИКОГДА не убывающий список, кратно
// умножающийся на каждый вызов. Фикс — решить ОДИН раз, есть ли вообще
// устройство на этих портах, и если нет — отказывать МГНОВЕННО, без
// единого busy-wait, вместо того чтобы каждый раз заново пытаться и
// платить за это реальным временем.
//
// НАЙДЕННЫЙ БАГ №3.1 (живым тестом, после №3): первая версия проверяла
// только "плавающую шину" (STATUS читается как 0xFF, когда к портам вообще
// ничего электрически не подключено) — но QEMU в "pc"-чипсете поднимает
// сам IDE-контроллер (PIIX) безусловно, даже без единого -drive if=ide, то
// есть STATUS отвечает НЕ 0xFF (чип есть), хотя реального диска за ним
// нет — "плавающая шина" ложно давала g_ata_present=1, и каждая запись
// всё равно тратила полный таймаут вместо мгновенного отказа (отсюда
// "xhci в nvme-режиме долго сканирует порты" — disk-операции, набегающие
// по ходу USB-энумерации, каждая платила ~1с). Настоящий тест — классика
// ATA-пробы (OSDev "ATA PIO Mode"): выбрать master (0xA0, LBA-биты
// неважны), записать заведомый паттерн в LBA_MID/LBA_HIGH и прочитать его
// обратно — эти регистры реально ЗАЩЁЛКИВАЕТ только существующее
// устройство; и плавающая шина, и отвечающий, но безликий контроллер без
// диска — ничего не защёлкивают, читаются независимо от записи.
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
        // Дожидаемся сброса BSY после записи — тот же тест (status & 0x80),
        // что и ata_wait_ready(), возврат как раньше не проверялся.
        ata_wait_ready();
    }
    return 0;
}