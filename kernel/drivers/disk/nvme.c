#include "nvme.h"
#include "drivers/pci/pci.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "drivers/console/console.h"
#include "lib/string.h"
#include "lib/stddef.h"

// NVMe-драйвер: см. общий разбор архитектуры в nvme.h. Регистры — MMIO по
// BAR0 (спецификацией он обязан быть 64-битным); доступ по тем же
// reg_read32/reg_write32, что и ahci.c (volatile-указатель прямо в память
// контроллера).

#define NVME_CLASS      0x01
#define NVME_SUBCLASS   0x08
#define NVME_PROGIF     0x02

/* ===== Controller регистры — смещения от BAR0 ===== */
#define NVME_REG_CAP   0x00 // 8 байт
#define NVME_REG_VS    0x08
#define NVME_REG_CC    0x14
#define NVME_REG_CSTS  0x1C
#define NVME_REG_AQA   0x24
#define NVME_REG_ASQ   0x28 // 8 байт
#define NVME_REG_ACQ   0x30 // 8 байт
#define NVME_DB_BASE   0x1000

#define NVME_CSTS_RDY  (1u << 0)

#define NVME_ADMIN_QDEPTH 2
#define NVME_IO_QDEPTH    2
#define NVME_BOUNCE_PAGES 32 // 128 КБ — см. разбор в nvme.h/ahci.c

static inline uint32_t reg_read32(void *base, uint32_t off) {
    return *(volatile uint32_t *)((uint8_t *)base + off);
}
static inline void reg_write32(void *base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)((uint8_t *)base + off) = val;
}
static inline void reg_write64(void *base, uint32_t off, uint64_t val) {
    reg_write32(base, off, (uint32_t)(val & 0xFFFFFFFFu));
    reg_write32(base, off + 4, (uint32_t)(val >> 32));
}

/* ===== Submission queue entry — 64 байта ===== */
typedef struct __attribute__((packed)) {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t cid;
    uint32_t nsid;
    uint32_t cdw2;
    uint32_t cdw3;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} nvme_cmd_t;

/* ===== Completion queue entry — 16 байт ===== */
typedef struct __attribute__((packed)) {
    uint32_t dw0;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status_phase; // бит0 — phase tag, биты1-15 — status field
} nvme_cqe_t;

static void *g_bar;
static uint32_t g_db_stride; // в байтах = 4 << CAP.DSTRD
static uint32_t g_nsid = 1;
static int g_ready = 0;

static nvme_cmd_t *g_admin_sq; static nvme_cqe_t *g_admin_cq;
static uint16_t g_admin_sq_tail = 0, g_admin_cq_head = 0; static uint8_t g_admin_phase = 0;

static nvme_cmd_t *g_io_sq; static nvme_cqe_t *g_io_cq;
static uint16_t g_io_sq_tail = 0, g_io_cq_head = 0; static uint8_t g_io_phase = 0;

static uint64_t g_bounce_phys; static uint8_t *g_bounce_virt;
static uint64_t g_prplist_phys; static uint64_t *g_prplist_virt;

static inline uint32_t sq_db_off(uint32_t qid) { return NVME_DB_BASE + (2 * qid) * g_db_stride; }
static inline uint32_t cq_db_off(uint32_t qid) { return NVME_DB_BASE + (2 * qid + 1) * g_db_stride; }

// Та же причина и тот же приём, что alloc_bounce_buffer() в ahci.c: нужен
// один ФИЗИЧЕСКИ непрерывный буфер под единственный PRP1 + PRP-list (а не
// честную scatter-gather разбивку чужого буфера на каждый вызов).
static int alloc_bounce_buffer(void) {
    uint64_t first = pmm_alloc_page();
    if (!first) return -1;
    uint64_t prev = first;
    for (int i = 1; i < NVME_BOUNCE_PAGES; i++) {
        uint64_t p = pmm_alloc_page();
        if (!p || p != prev + PAGE_SIZE) {
            printf("[NVMe] bounce buffer not physically contiguous, disabling NVMe\n");
            return -1;
        }
        prev = p;
    }
    g_bounce_phys = first;
    g_bounce_virt = (uint8_t *)phys_to_virt(first);
    return 0;
}

static void submit_cmd(nvme_cmd_t *sq, uint16_t *tail, uint16_t depth, uint32_t db_off, const nvme_cmd_t *cmd) {
    sq[*tail] = *cmd;
    *tail = (uint16_t)(*tail + 1);
    if (*tail == depth) *tail = 0;
    reg_write32(g_bar, db_off, *tail);
}

// Поллит phase-бит записи completion queue под текущей head (ждём, пока
// контроллер не выставит его равным нашему ожидаемому значению — ровно тот
// же busy-wait стиль, что ahci_issue_command()'овское ожидание PxCI), затем
// продвигает head/phase и отдаёт её обратно контроллеру через doorbell.
// 0 — успешное завершение (status field == 0), -1 — ошибка или таймаут.
static int wait_completion(nvme_cqe_t *cq, uint16_t *head, uint8_t *phase, uint16_t depth, uint32_t cq_db_off_val) {
    for (volatile int i = 0; i < 20000000; i++) {
        uint16_t sp = cq[*head].status_phase;
        if ((uint16_t)(sp & 1u) == (uint16_t)*phase) {
            uint16_t status = (uint16_t)(sp >> 1);
            *head = (uint16_t)(*head + 1);
            if (*head == depth) { *head = 0; *phase = (uint8_t)(*phase ^ 1); }
            reg_write32(g_bar, cq_db_off_val, *head);
            return status == 0 ? 0 : -1;
        }
    }
    return -1; // таймаут — контроллер не ответил
}

static int admin_command(const nvme_cmd_t *cmd) {
    submit_cmd(g_admin_sq, &g_admin_sq_tail, NVME_ADMIN_QDEPTH, sq_db_off(0), cmd);
    return wait_completion(g_admin_cq, &g_admin_cq_head, &g_admin_phase, NVME_ADMIN_QDEPTH, cq_db_off(0));
}

static int io_command(const nvme_cmd_t *cmd) {
    submit_cmd(g_io_sq, &g_io_sq_tail, NVME_IO_QDEPTH, sq_db_off(1), cmd);
    return wait_completion(g_io_cq, &g_io_cq_head, &g_io_phase, NVME_IO_QDEPTH, cq_db_off(1));
}

int nvme_available(void) {
    return g_ready;
}

int nvme_init(void) {
    const pci_device_t *dev = pci_find_class_if(NVME_CLASS, NVME_SUBCLASS, NVME_PROGIF);
    if (!dev) return -1;

    pci_enable_memory(dev);
    pci_enable_bus_master(dev);
    pci_disable_interrupts(dev); // опрашиваем сами — см. nvme.h

    pci_bar_t bar;
    if (pci_get_bar(dev, 0, &bar) != 0 || bar.is_io) {
        printf("[NVMe] BAR0 missing or not MMIO\n");
        return -1;
    }

    uint64_t map_size = bar.size;
    if (map_size == 0) map_size = 0x2000;
    if (map_size > 0x100000) map_size = 0x100000; // разумный потолок — doorbell'ы нам нужны только под QID 0/1

    g_bar = mmio_map(bar.address, map_size);
    if (!g_bar) {
        printf("[NVMe] Failed to map BAR0\n");
        return -1;
    }

    uint32_t cap_lo = reg_read32(g_bar, NVME_REG_CAP);
    uint32_t cap_hi = reg_read32(g_bar, NVME_REG_CAP + 4);
    (void)cap_lo;
    uint32_t dstrd = cap_hi & 0xF;
    uint32_t mpsmin = (cap_hi >> 16) & 0xF;
    if (mpsmin != 0) {
        printf("[NVMe] Controller requires page size > 4K, unsupported\n");
        return -1;
    }
    g_db_stride = 4u << dstrd;

    // Если контроллер уже включён (не ожидается на свежем старте, но
    // бывает после кривого firmware) — гасим и ждём RDY=0 перед тем, как
    // переписывать admin-очереди, иначе можно гонка с уже идущим DMA.
    if (reg_read32(g_bar, NVME_REG_CSTS) & NVME_CSTS_RDY) {
        reg_write32(g_bar, NVME_REG_CC, 0);
        int ok = 0;
        for (volatile int i = 0; i < 2000000; i++) {
            if (!(reg_read32(g_bar, NVME_REG_CSTS) & NVME_CSTS_RDY)) { ok = 1; break; }
        }
        if (!ok) { printf("[NVMe] Controller did not disable in time\n"); return -1; }
    }

    uint64_t admin_sq_phys = pmm_alloc_page();
    uint64_t admin_cq_phys = pmm_alloc_page();
    uint64_t io_sq_phys = pmm_alloc_page();
    uint64_t io_cq_phys = pmm_alloc_page();
    uint64_t ident_phys = pmm_alloc_page();
    uint64_t prplist_phys = pmm_alloc_page();
    if (!admin_sq_phys || !admin_cq_phys || !io_sq_phys || !io_cq_phys || !ident_phys || !prplist_phys) {
        printf("[NVMe] Out of memory setting up queues\n");
        return -1;
    }
    g_admin_sq = (nvme_cmd_t *)phys_to_virt(admin_sq_phys);
    g_admin_cq = (nvme_cqe_t *)phys_to_virt(admin_cq_phys);
    g_io_sq = (nvme_cmd_t *)phys_to_virt(io_sq_phys);
    g_io_cq = (nvme_cqe_t *)phys_to_virt(io_cq_phys);
    g_prplist_phys = prplist_phys;
    g_prplist_virt = (uint64_t *)phys_to_virt(prplist_phys);
    memset(g_admin_sq, 0, PAGE_SIZE);
    memset(g_admin_cq, 0, PAGE_SIZE);
    memset(g_io_sq, 0, PAGE_SIZE);
    memset(g_io_cq, 0, PAGE_SIZE);
    memset(g_prplist_virt, 0, PAGE_SIZE);

    if (alloc_bounce_buffer() != 0) return -1;
    // Буфер физически неподвижен — список страниц 2..32 заполняется раз и
    // навсегда (а не на каждый I/O, как PRDT у AHCI, который перезаписывает
    // только одно поле count/adress — тут экономим даже это).
    for (int i = 0; i < NVME_BOUNCE_PAGES - 1; i++)
        g_prplist_virt[i] = g_bounce_phys + (uint64_t)(i + 1) * PAGE_SIZE;

    g_admin_sq_tail = g_admin_cq_head = 0; g_admin_phase = 0;
    g_io_sq_tail = g_io_cq_head = 0; g_io_phase = 0;

    reg_write32(g_bar, NVME_REG_AQA, (NVME_ADMIN_QDEPTH - 1) | ((NVME_ADMIN_QDEPTH - 1) << 16));
    reg_write64(g_bar, NVME_REG_ASQ, admin_sq_phys);
    reg_write64(g_bar, NVME_REG_ACQ, admin_cq_phys);

    // CC: EN=1, CSS=0 (NVM command set), MPS=0 (4КБ страницы), AMS=0,
    // SHN=0, IOSQES=6 (2^6=64 байта — наш nvme_cmd_t), IOCQES=4 (2^4=16 —
    // наш nvme_cqe_t).
    uint32_t cc = 1u | (6u << 16) | (4u << 20);
    reg_write32(g_bar, NVME_REG_CC, cc);

    int ready = 0;
    for (volatile int i = 0; i < 2000000; i++) {
        if (reg_read32(g_bar, NVME_REG_CSTS) & NVME_CSTS_RDY) { ready = 1; break; }
    }
    if (!ready) {
        printf("[NVMe] Controller did not become ready\n");
        return -1;
    }

    // Identify Namespace (CNS=0x00, NSID=1) — проверяем, что размер LBA
    // ровно 512 байт без metadata; иначе наш disk.c-совместимый API (как и
    // AHCI) просто не подойдёт, откатываемся без попытки дальше.
    uint8_t *ident = (uint8_t *)phys_to_virt(ident_phys);
    memset(ident, 0, PAGE_SIZE);
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = 0x06; // Identify
    cmd.cid = 1;
    cmd.nsid = g_nsid;
    cmd.prp1 = ident_phys;
    cmd.cdw10 = 0x00; // CNS=0 — Identify Namespace

    if (admin_command(&cmd) != 0) {
        printf("[NVMe] Identify Namespace failed (no namespace 1?)\n");
        return -1;
    }

    uint8_t flbas = ident[26] & 0x0F;
    uint32_t lbaf_off = 128 + (uint32_t)flbas * 4;
    uint16_t ms = *(uint16_t *)(ident + lbaf_off);
    uint8_t lbads = ident[lbaf_off + 2];
    if (ms != 0 || lbads != 9) { // 2^9 = 512
        printf("[NVMe] Namespace LBA size unsupported (need 512B, no metadata)\n");
        return -1;
    }

    // Создаём I/O очереди (QID 1): сперва completion, затем submission —
    // SQ ссылается на CQID, которая обязана уже существовать.
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = 0x05; // Create I/O Completion Queue
    cmd.cid = 1;
    cmd.prp1 = io_cq_phys;
    cmd.cdw10 = 1u | (((uint32_t)(NVME_IO_QDEPTH - 1)) << 16); // QID=1, QSIZE-1
    cmd.cdw11 = 1u; // PC=1 (physically contiguous), IEN=0 — не используем прерывания
    if (admin_command(&cmd) != 0) {
        printf("[NVMe] Create I/O CQ failed\n");
        return -1;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = 0x01; // Create I/O Submission Queue
    cmd.cid = 1;
    cmd.prp1 = io_sq_phys;
    cmd.cdw10 = 1u | (((uint32_t)(NVME_IO_QDEPTH - 1)) << 16); // QID=1, QSIZE-1
    cmd.cdw11 = 1u | (1u << 16); // PC=1, CQID=1
    if (admin_command(&cmd) != 0) {
        printf("[NVMe] Create I/O SQ failed\n");
        return -1;
    }

    g_ready = 1;
    printf("[NVMe] Namespace 1 ready (512B LBA)\n");
    return 0;
}

static int nvme_issue_rw(uint32_t lba, uint16_t count, int is_write) {
    nvme_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = is_write ? 0x01 : 0x02; // Write / Read
    cmd.cid = 1;
    cmd.nsid = g_nsid;
    cmd.prp1 = g_bounce_phys;
    uint32_t total = (uint32_t)count * 512u;
    cmd.prp2 = (total > PAGE_SIZE) ? g_prplist_phys : 0;
    cmd.cdw10 = lba; // SLBA, младшие 32 бита (мы всегда работаем в пределах 32-битного API disk.c)
    cmd.cdw11 = 0;
    cmd.cdw12 = (uint32_t)(count - 1); // NLB — 0-based

    return io_command(&cmd);
}

int nvme_read_sectors(uint32_t lba, uint8_t sector_count, void *buffer) {
    if (!g_ready || sector_count == 0) return -1;
    if (nvme_issue_rw(lba, sector_count, 0) != 0) return -1;
    memcpy(buffer, g_bounce_virt, (uint32_t)sector_count * 512u);
    return 0;
}

int nvme_write_sectors(uint32_t lba, uint8_t sector_count, const void *buffer) {
    if (!g_ready || sector_count == 0) return -1;
    memcpy(g_bounce_virt, buffer, (uint32_t)sector_count * 512u);
    if (nvme_issue_rw(lba, sector_count, 1) != 0) return -1;
    return 0;
}
