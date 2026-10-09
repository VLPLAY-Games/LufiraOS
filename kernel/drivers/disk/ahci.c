#include "ahci.h"
#include "drivers/pci/pci.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "drivers/console/console.h"
#include "lib/string.h"
#include "lib/stddef.h"

// AHCI-драйвер: один контроллер, один порт (первый найденный с реально
// подключённым SATA-диском), одна команда одновременно (слот 0), опрос
// PxCI вместо прерываний — тот же выбор, что уже сделан для xHCI/RTL8139/
// AC'97 в этом ядре (см. их собственные комментарии "PCI IRQ не
// используем"/"нет MSI-X"): здесь ровно по той же причине — у ядра в
// принципе нет парсинга PCI capability list под MSI/MSI-X, а делить общий
// legacy INTx с другими устройствами ради одного диска, читаемого раз в
// несколько секунд на запись "грязных" блоков LufiraFS, не стоит сложности.
//
// Единственный потребитель этого драйвера сегодня — disk.c (замена/
// дополнение легаси ATA PIO для lufirafs_sync()'а, см. ahci.h). Полноценная
// scatter-gather DMA под произвольный (возможно физически фрагментированный)
// буфер вызывающего не реализована — вместо этого один заранее выделенный,
// гарантированно физически непрерывный bounce-буфер (128 КБ — с запасом
// перекрывает максимум disk_read_sectors()/disk_write_sectors(): 255 секторов
// * 512 байт = 124.5 КБ), через который read/write всегда идут memcpy()'ем.
// Проще и безопаснее, чем проверять физическую непрерывность ЧУЖОГО
// (heap-выделенного) буфера на каждый вызов.

#define AHCI_CLASS      0x01
#define AHCI_SUBCLASS   0x06
#define AHCI_PROGIF     0x01

/* ===== HBA (global) регистры — смещения от ABAR (BAR5) ===== */
#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_IS    0x08
#define HBA_PI    0x0C

#define GHC_AE    (1u << 31)
#define GHC_HR    (1u << 0)

/* ===== Port регистры — смещения от (ABAR + 0x100 + port*0x80) ===== */
#define PX_CLB    0x00
#define PX_CLBU   0x04
#define PX_FB     0x08
#define PX_FBU    0x0C
#define PX_IS     0x10
#define PX_IE     0x14
#define PX_CMD    0x18
#define PX_TFD    0x20
#define PX_SIG    0x24
#define PX_SSTS   0x28
#define PX_SCTL   0x2C
#define PX_SERR   0x30
#define PX_CI     0x38

#define PXCMD_ST   (1u << 0)
#define PXCMD_FRE  (1u << 4)
#define PXCMD_FR   (1u << 14)
#define PXCMD_CR   (1u << 15)

#define PXTFD_ERR  (1u << 0)
#define PXTFD_DRQ  (1u << 3)
#define PXTFD_BSY  (1u << 7)

#define PXIS_TFES  (1u << 30)

#define SATA_SIG_ATA 0x00000101u

static inline uint32_t reg_read32(void *base, uint32_t off) {
    return *(volatile uint32_t *)((uint8_t *)base + off);
}
static inline void reg_write32(void *base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)((uint8_t *)base + off) = val;
}

/* ===== Command List Header — 32 байт, 32 штуки подряд = 1КБ (1КБ-выровнено) */
typedef struct __attribute__((packed)) {
    uint16_t flags;  // CFL(5) | A(1) | W(1) | P(1) | R(1) | B(1) | C(1) | rsv(1) | PMP(4)
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv[4];
} hba_cmd_header_t;

/* ===== PRDT entry — 16 байт ===== */
typedef struct __attribute__((packed)) {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc_i; // биты 0-21: байт-count - 1; бит 31: interrupt on completion
} hba_prdt_entry_t;

/* ===== Command Table — заголовок 0x80 байт + PRDT'ы (нам хватает одной) */
typedef struct __attribute__((packed)) {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    hba_prdt_entry_t prdt[1];
} hba_cmd_table_t;

/* ===== Register FIS (Host to Device) — 20 байт ===== */
typedef struct __attribute__((packed)) {
    uint8_t fis_type; // 0x27
    uint8_t pm_port_c; // бит7 = Command
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0, lba1, lba2;
    uint8_t device;
    uint8_t lba3, lba4, lba5;
    uint8_t featureh;
    uint8_t countl, counth;
    uint8_t icc;
    uint8_t control;
    uint8_t rsv[4];
} fis_reg_h2d_t;

#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35

#define AHCI_BOUNCE_PAGES 32 // 128 КБ — см. разбор вверху файла

static void *g_abar;
static void *g_port_base;
static hba_cmd_header_t *g_cmd_list;
static hba_cmd_table_t *g_cmd_table;
static uint64_t g_bounce_phys;
static uint8_t *g_bounce_virt;
static int g_ready = 0;

// Выделяет AHCI_BOUNCE_PAGES страниц, требуя, чтобы они оказались физически
// подряд (иначе один PRDT-entry не покрыл бы весь буфер корректно) — на
// старте, когда физическая память ещё не фрагментирована, pmm_alloc_page()
// в этом ядре отдаёт страницы последовательно, так что это обычно
// срабатывает с первой попытки; если нет — AHCI просто остаётся
// недоступен (disk.c откатывается на легаси ATA PIO), без сложной логики
// повторных попыток/компактации.
static int alloc_bounce_buffer(void) {
    uint64_t first = pmm_alloc_page();
    if (!first) return -1;

    uint64_t prev = first;
    for (int i = 1; i < AHCI_BOUNCE_PAGES; i++) {
        uint64_t p = pmm_alloc_page();
        if (!p || p != prev + PAGE_SIZE) {
            printf("[AHCI] bounce buffer not physically contiguous, disabling AHCI\n");
            return -1;
        }
        prev = p;
    }

    g_bounce_phys = first;
    g_bounce_virt = (uint8_t *)phys_to_virt(first);
    return 0;
}

// Останавливает движок порта (ST/FRE) и ждёт, пока контроллер подтвердит
// остановку (CR/FR сброшены) — обязательно ПЕРЕД перенастройкой PxCLB/PxFB,
// иначе контроллер мог бы продолжать читать/писать по старым (или ещё не
// готовым) адресам.
static int port_stop(void *port) {
    uint32_t cmd = reg_read32(port, PX_CMD);
    cmd &= ~(PXCMD_ST | PXCMD_FRE);
    reg_write32(port, PX_CMD, cmd);

    for (volatile int i = 0; i < 1000000; i++) {
        if (!(reg_read32(port, PX_CMD) & (PXCMD_CR | PXCMD_FR))) return 0;
    }
    return -1;
}

static void port_start(void *port) {
    uint32_t cmd = reg_read32(port, PX_CMD);
    cmd |= PXCMD_FRE;
    reg_write32(port, PX_CMD, cmd);
    cmd |= PXCMD_ST;
    reg_write32(port, PX_CMD, cmd);
}

int ahci_available(void) {
    return g_ready;
}

int ahci_init(void) {
    const pci_device_t *dev = pci_find_class_if(AHCI_CLASS, AHCI_SUBCLASS, AHCI_PROGIF);
    if (!dev) return -1;

    pci_enable_memory(dev);
    pci_enable_bus_master(dev);
    pci_disable_interrupts(dev); // опрашиваем сами — см. разбор вверху файла

    pci_bar_t bar;
    if (pci_get_bar(dev, 5, &bar) != 0 || bar.is_io) {
        printf("[AHCI] BAR5 (ABAR) missing or not MMIO\n");
        return -1;
    }

    g_abar = mmio_map(bar.address, bar.size);
    if (!g_abar) {
        printf("[AHCI] Failed to map ABAR\n");
        return -1;
    }

    // AE — некоторые контроллеры стартуют уже в AHCI-режиме, но не все
    // прошивки это гарантируют; HR (HBA reset) сознательно не трогаем —
    // полный сброс контроллера за пределами того, что нужно одному диску.
    reg_write32(g_abar, HBA_GHC, reg_read32(g_abar, HBA_GHC) | GHC_AE);

    uint32_t pi = reg_read32(g_abar, HBA_PI);
    void *port = NULL;
    int port_index = -1;

    for (int i = 0; i < 32 && port_index < 0; i++) {
        if (!(pi & (1u << i))) continue;

        void *p = (uint8_t *)g_abar + 0x100 + i * 0x80;
        uint32_t ssts = reg_read32(p, PX_SSTS);
        uint32_t det = ssts & 0x0F;
        if (det != 3) continue; // устройство не подключено / PHY не поднят

        uint32_t sig = reg_read32(p, PX_SIG);
        if (sig != SATA_SIG_ATA) continue; // ATAPI/прочее — не обслуживаем

        port = p;
        port_index = i;
    }

    if (!port) {
        printf("[AHCI] No active SATA port found\n");
        return -1;
    }

    if (port_stop(port) != 0) {
        printf("[AHCI] Port %d did not stop in time\n", port_index);
        return -1;
    }

    uint64_t cmd_list_phys = pmm_alloc_page();
    uint64_t fis_phys = pmm_alloc_page();
    uint64_t cmd_table_phys = pmm_alloc_page();
    if (!cmd_list_phys || !fis_phys || !cmd_table_phys) {
        printf("[AHCI] Out of memory setting up port %d\n", port_index);
        return -1;
    }
    // По странице на структуру — все требования выравнивания (1КБ/256Б/128Б)
    // тривиально выполняются 4КБ-страницей; места под единственный слот
    // команд (мы используем только слот 0) и одну command table с запасом.
    g_cmd_list = (hba_cmd_header_t *)phys_to_virt(cmd_list_phys);
    g_cmd_table = (hba_cmd_table_t *)phys_to_virt(cmd_table_phys);
    memset(g_cmd_list, 0, PAGE_SIZE);
    memset((void *)phys_to_virt(fis_phys), 0, PAGE_SIZE);
    memset(g_cmd_table, 0, PAGE_SIZE);

    if (alloc_bounce_buffer() != 0) {
        return -1;
    }

    reg_write32(port, PX_CLB, (uint32_t)(cmd_list_phys & 0xFFFFFFFF));
    reg_write32(port, PX_CLBU, (uint32_t)(cmd_list_phys >> 32));
    reg_write32(port, PX_FB, (uint32_t)(fis_phys & 0xFFFFFFFF));
    reg_write32(port, PX_FBU, (uint32_t)(fis_phys >> 32));

    reg_write32(port, PX_SERR, 0xFFFFFFFF); // write-1-to-clear
    reg_write32(port, PX_IS, 0xFFFFFFFF);

    g_cmd_list[0].ctba = (uint32_t)(cmd_table_phys & 0xFFFFFFFF);
    g_cmd_list[0].ctbau = (uint32_t)(cmd_table_phys >> 32);
    g_cmd_list[0].prdtl = 1;

    port_start(port);

    g_port_base = port;
    g_ready = 1;

    printf("[AHCI] Port %d ready (SATA disk found)\n", port_index);
    return 0;
}

// Строит FIS + PRDT под слот 0, бьёт PxCI и ждёт её сброса (контроллер сам
// снимает бит, когда команда отработала) — тот же стиль busy-wait с
// лимитом итераций, что уже использует disk.c для легаси ATA PIO (ata_wait_*()),
// просто с другим количеством итераций под DMA, которое обычно быстрее PIO.
static int ahci_issue_command(uint32_t lba, uint16_t count, int is_write) {
    for (volatile int i = 0; i < 10000000; i++) {
        uint32_t tfd = reg_read32(g_port_base, PX_TFD);
        if (!(tfd & (PXTFD_BSY | PXTFD_DRQ))) break;
        if (i == 9999999) return -1; // контроллер завис на предыдущей команде
    }

    g_cmd_list[0].flags = (uint16_t)(5 | (is_write ? (1u << 6) : 0)); // CFL=5 (dwords в FIS), W=бит6
    g_cmd_list[0].prdtl = 1;
    g_cmd_list[0].prdbc = 0;

    g_cmd_table->prdt[0].dba = (uint32_t)(g_bounce_phys & 0xFFFFFFFF);
    g_cmd_table->prdt[0].dbau = (uint32_t)(g_bounce_phys >> 32);
    g_cmd_table->prdt[0].dbc_i = (((uint32_t)count * 512u) - 1u) | (1u << 31);

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)g_cmd_table->cfis;
    memset(fis, 0, sizeof(*fis));
    fis->fis_type = 0x27;
    fis->pm_port_c = 0x80; // C=1 — это команда, не статус
    fis->command = is_write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    fis->device = 0x40; // LBA mode
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = 0; // LBA48 верхние 16 бит — lba у нас только 32-битный, всегда 0
    fis->lba5 = 0;
    fis->countl = (uint8_t)(count & 0xFF);
    fis->counth = (uint8_t)((count >> 8) & 0xFF);

    reg_write32(g_port_base, PX_SERR, 0xFFFFFFFF);
    reg_write32(g_port_base, PX_IS, 0xFFFFFFFF);
    reg_write32(g_port_base, PX_CI, 1u);

    for (volatile int i = 0; i < 20000000; i++) {
        if (!(reg_read32(g_port_base, PX_CI) & 1u)) {
            if (reg_read32(g_port_base, PX_IS) & PXIS_TFES) return -1;
            if (reg_read32(g_port_base, PX_TFD) & PXTFD_ERR) return -1;
            return 0;
        }
    }
    return -1; // таймаут — команда не завершилась
}

int ahci_read_sectors(uint32_t lba, uint8_t sector_count, void *buffer) {
    if (!g_ready || sector_count == 0) return -1;
    if (ahci_issue_command(lba, sector_count, 0) != 0) return -1;
    memcpy(buffer, g_bounce_virt, (uint32_t)sector_count * 512u);
    return 0;
}

int ahci_write_sectors(uint32_t lba, uint8_t sector_count, const void *buffer) {
    if (!g_ready || sector_count == 0) return -1;
    memcpy(g_bounce_virt, buffer, (uint32_t)sector_count * 512u);
    if (ahci_issue_command(lba, sector_count, 1) != 0) return -1;
    return 0;
}
