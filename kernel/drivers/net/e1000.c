#include "e1000.h"
#include "net/eth.h"

#include "drivers/pci/pci.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "system/timer/pit.h"
#include "drivers/console/console.h"
#include "lib/string.h"
#include "system/devmode/devmode.h"
#include "system/klog/klog.h"

/* ======================================================================== */
/* Регистры (смещения от MMIO base, см. e1000_find_controller())            */
/* ======================================================================== */

#define E1000_REG_CTRL     0x0000
#define E1000_REG_STATUS   0x0008
#define E1000_REG_EERD     0x0014
#define E1000_REG_ICR      0x00C0
#define E1000_REG_IMS      0x00D0
#define E1000_REG_IMC      0x00D8
#define E1000_REG_RCTL     0x0100
#define E1000_REG_TCTL     0x0400
#define E1000_REG_TIPG     0x0410
#define E1000_REG_RDBAL    0x2800
#define E1000_REG_RDBAH    0x2804
#define E1000_REG_RDLEN    0x2808
#define E1000_REG_RDH      0x2810
#define E1000_REG_RDT      0x2818
#define E1000_REG_TDBAL    0x3800
#define E1000_REG_TDBAH    0x3804
#define E1000_REG_TDLEN    0x3808
#define E1000_REG_TDH      0x3810
#define E1000_REG_TDT      0x3818
#define E1000_REG_RAL0     0x5400
#define E1000_REG_RAH0     0x5404

#define E1000_CTRL_SLU   (1u << 6)  // Set Link Up
#define E1000_CTRL_RST   (1u << 26)

#define E1000_EERD_START (1u << 0)
#define E1000_EERD_DONE  (1u << 4)
#define E1000_EERD_ADDR_SHIFT 8
#define E1000_EERD_DATA_SHIFT 16

#define E1000_RCTL_EN    (1u << 1)
#define E1000_RCTL_BAM   (1u << 15) // Broadcast Accept Mode
#define E1000_RCTL_SECRC (1u << 26) // карта сама срезает 4-байтный Ethernet CRC

#define E1000_TCTL_EN    (1u << 1)
#define E1000_TCTL_PSP   (1u << 3) // Pad Short Packets (до минимума 60 байт)
#define E1000_TCTL_CT_SHIFT   4
#define E1000_TCTL_COLD_SHIFT 12

#define E1000_RAH_AV     (1u << 31) // Address Valid

#define E1000_RX_STATUS_DD  (1u << 0) // Descriptor Done
#define E1000_TX_CMD_EOP    (1u << 0)
#define E1000_TX_CMD_IFCS   (1u << 1)
#define E1000_TX_CMD_RS     (1u << 3) // Report Status (взвести DD по завершению)
#define E1000_TX_STATUS_DD  (1u << 0)

/* ======================================================================== */
/* Дескрипторы (legacy, 16 байт) — см. Intel 8254x Software Developer's     */
/* Manual, §3.3 (RX) / §3.4 (TX).                                           */
/* ======================================================================== */

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} e1000_rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} e1000_tx_desc_t;

#define E1000_RX_DESC_COUNT 32
#define E1000_TX_DESC_COUNT 8
#define E1000_POLL_MAX_PACKETS_PER_TICK 8

/* ======================================================================== */
/* Состояние драйвера                                                       */
/* ======================================================================== */

// Тот же cli/sti-guard и "уже выполняюсь, выйти", что в rtl8139.c (см.
// rtl_poll_busy) — e1000_poll()/e1000_send() вызываются и из IRQ (IF=0), и
// синхронно из обычного кода (IF=1).
static inline uint64_t e1000_cli_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) :: "memory");
    __asm__ volatile ("cli");
    return flags;
}
static inline void e1000_sti_restore(uint64_t flags) {
    __asm__ volatile ("push %0; popfq" : : "r"(flags) : "memory", "cc");
}
static volatile int e1000_poll_busy = 0;

static int       e1000_found_flag = 0;
static volatile uint8_t *e1000_regs = NULL;
static uint8_t    e1000_mac[6];

static volatile e1000_rx_desc_t *e1000_rx_ring = NULL;
static uint64_t   e1000_rx_ring_phys = 0;
static uint8_t    *e1000_rx_buf_virt[E1000_RX_DESC_COUNT];
static uint32_t    e1000_rx_tail = 0; // следующий дескриптор, который проверим

static volatile e1000_tx_desc_t *e1000_tx_ring = NULL;
static uint64_t   e1000_tx_ring_phys = 0;
static uint8_t    *e1000_tx_buf_virt[E1000_TX_DESC_COUNT];
static uint64_t    e1000_tx_buf_phys[E1000_TX_DESC_COUNT];
static uint32_t    e1000_tx_next = 0;

/* ======================================================================== */
/* Доступ к регистрам                                                       */
/* ======================================================================== */

static inline uint32_t e1000_read(uint32_t offset) {
    return *(volatile uint32_t *)(e1000_regs + offset);
}
static inline void e1000_write(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(e1000_regs + offset) = value;
}

/* ======================================================================== */
/* PCI-обнаружение                                                          */
/* ======================================================================== */

// Не исчерпывающий список — достаточно для QEMU (`-device e1000` =
// 82540EM, 0x100E) и распространённых классических карт семейства.
// ICH8-10/82571+ (PCIe, "e1000e") сюда намеренно не входят — см. e1000e.c.
static const uint16_t E1000_DEVICE_IDS[] = {
    0x100E, // 82540EM (QEMU "e1000")
    0x1004, // 82543GC (fiber)
    0x1008, // 82544EI
    0x1009, // 82544EI (fiber)
    0x100F, // 82545EM
    0x1010, // 82546EB
    0x1011, // 82545EM (fiber)
    0x1012, // 82546EB (fiber)
    0x101D, // 82546EB
    0x1026, // 82545GM
    0x1027, // 82545GM
    0x1028, // 82545GM
};
#define E1000_DEVICE_ID_COUNT (sizeof(E1000_DEVICE_IDS) / sizeof(E1000_DEVICE_IDS[0]))

static int e1000_find_controller(void) {
    const pci_device_t *dev = NULL;
    for (uint32_t i = 0; i < E1000_DEVICE_ID_COUNT && !dev; i++) {
        dev = pci_find_device(0x8086, E1000_DEVICE_IDS[i]);
    }
    if (!dev) {
        DLOG("[E1000] Controller not found\n");
        return 0;
    }

    DLOG("[E1000] Found at %u:%u.%u (device id 0x%x)\n",
         dev->bus, dev->device, dev->function, dev->device_id);

    pci_bar_t bar0;
    if (pci_get_bar(dev, 0, &bar0) != 0 || bar0.is_io) {
        printf("[E1000] Expected MMIO BAR0\n");
        return 0;
    }

    e1000_regs = (volatile uint8_t *)mmio_map(bar0.address, bar0.size);
    if (!e1000_regs) {
        printf("[E1000] mmio_map failed\n");
        return 0;
    }
    DLOG("[E1000] MMIO base = 0x%lx (phys 0x%lx, size 0x%lx)\n",
         (uint64_t)(uintptr_t)e1000_regs, bar0.address, bar0.size);

    pci_enable_memory(dev);
    pci_enable_bus_master(dev);
    pci_disable_interrupts(dev); // опрашиваем сами из net_poll(), см. заголовок файла

    return 1;
}

/* ======================================================================== */
/* Сброс / EEPROM / настройка                                               */
/* ======================================================================== */

static int e1000_reset(void) {
    e1000_write(E1000_REG_CTRL, e1000_read(E1000_REG_CTRL) | E1000_CTRL_RST);

    for (int i = 0; i < 1000; i++) {
        if (!(e1000_read(E1000_REG_CTRL) & E1000_CTRL_RST)) {
            DLOG("[E1000] Reset complete\n");
            // Маскируем прерывания и подтверждаем защёлкнувшееся во время
            // сброса — стандартный шаг датащита после RST.
            e1000_write(E1000_REG_IMC, 0xFFFFFFFFu);
            e1000_read(E1000_REG_ICR);
            return 1;
        }
        pit_wait_ms(1);
    }

    printf("[E1000] Reset timed out\n");
    return 0;
}

static uint16_t e1000_eeprom_read_word(uint8_t addr) {
    e1000_write(E1000_REG_EERD, ((uint32_t)addr << E1000_EERD_ADDR_SHIFT) | E1000_EERD_START);

    uint32_t val = 0;
    for (int i = 0; i < 100000; i++) {
        val = e1000_read(E1000_REG_EERD);
        if (val & E1000_EERD_DONE) break;
    }
    return (uint16_t)(val >> E1000_EERD_DATA_SHIFT);
}

static void e1000_read_mac(void) {
    uint16_t w0 = e1000_eeprom_read_word(0);
    uint16_t w1 = e1000_eeprom_read_word(1);
    uint16_t w2 = e1000_eeprom_read_word(2);

    e1000_mac[0] = (uint8_t)(w0 & 0xFF);
    e1000_mac[1] = (uint8_t)(w0 >> 8);
    e1000_mac[2] = (uint8_t)(w1 & 0xFF);
    e1000_mac[3] = (uint8_t)(w1 >> 8);
    e1000_mac[4] = (uint8_t)(w2 & 0xFF);
    e1000_mac[5] = (uint8_t)(w2 >> 8);

    // Прописываем явно в RAL0/RAH0 (Address Valid), не полагаясь на то, что
    // карта сама перенесла EEPROM в эти регистры при сбросе.
    uint32_t ral = ((uint32_t)e1000_mac[3] << 24) | ((uint32_t)e1000_mac[2] << 16) |
                   ((uint32_t)e1000_mac[1] << 8) | (uint32_t)e1000_mac[0];
    uint32_t rah = E1000_RAH_AV | ((uint32_t)e1000_mac[5] << 8) | (uint32_t)e1000_mac[4];
    e1000_write(E1000_REG_RAL0, ral);
    e1000_write(E1000_REG_RAH0, rah);
}

static int e1000_setup_rx(void) {
    uint64_t ring_phys = pmm_alloc_page(); // 32 * 16 байт = 512 — влезает в одну страницу
    if (!ring_phys) { printf("[E1000] RX ring alloc failed\n"); return 0; }
    e1000_rx_ring_phys = ring_phys;
    e1000_rx_ring = (volatile e1000_rx_desc_t *)phys_to_virt(ring_phys);
    memset((void *)(uintptr_t)e1000_rx_ring, 0, PAGE_SIZE);

    for (int i = 0; i < E1000_RX_DESC_COUNT; i++) {
        uint64_t buf_phys = pmm_alloc_page();
        if (!buf_phys) { printf("[E1000] RX buffer %d alloc failed\n", i); return 0; }
        e1000_rx_buf_virt[i] = (uint8_t *)phys_to_virt(buf_phys);
        e1000_rx_ring[i].addr = buf_phys;
        e1000_rx_ring[i].status = 0;
    }
    e1000_rx_tail = 0;

    e1000_write(E1000_REG_RDBAL, (uint32_t)ring_phys);
    e1000_write(E1000_REG_RDBAH, (uint32_t)(ring_phys >> 32));
    e1000_write(E1000_REG_RDLEN, E1000_RX_DESC_COUNT * sizeof(e1000_rx_desc_t));
    e1000_write(E1000_REG_RDH, 0);
    // RDT = count-1: все дескрипторы отданы карте, кроме одного —
    // стандартная идиома кольца (Intel SDM §3.2.6).
    e1000_write(E1000_REG_RDT, E1000_RX_DESC_COUNT - 1);

    // BSIZE/BSEX не трогаем (оба поля по умолчанию 0 -> 2048 байт на буфер,
    // с запасом хватает на обычный Ethernet-кадр 1518 байт без jumbo).
    e1000_write(E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);
    return 1;
}

static int e1000_setup_tx(void) {
    uint64_t ring_phys = pmm_alloc_page(); // 8 * 16 байт = 128 — тоже одна страница
    if (!ring_phys) { printf("[E1000] TX ring alloc failed\n"); return 0; }
    e1000_tx_ring_phys = ring_phys;
    e1000_tx_ring = (volatile e1000_tx_desc_t *)phys_to_virt(ring_phys);
    memset((void *)(uintptr_t)e1000_tx_ring, 0, PAGE_SIZE);

    for (int i = 0; i < E1000_TX_DESC_COUNT; i++) {
        uint64_t buf_phys = pmm_alloc_page();
        if (!buf_phys) { printf("[E1000] TX buffer %d alloc failed\n", i); return 0; }
        e1000_tx_buf_phys[i] = buf_phys;
        e1000_tx_buf_virt[i] = (uint8_t *)phys_to_virt(buf_phys);
        e1000_tx_ring[i].status = E1000_TX_STATUS_DD; // свободен с самого начала
    }
    e1000_tx_next = 0;

    e1000_write(E1000_REG_TDBAL, (uint32_t)ring_phys);
    e1000_write(E1000_REG_TDBAH, (uint32_t)(ring_phys >> 32));
    e1000_write(E1000_REG_TDLEN, E1000_TX_DESC_COUNT * sizeof(e1000_tx_desc_t));
    e1000_write(E1000_REG_TDH, 0);
    e1000_write(E1000_REG_TDT, 0);

    // Значения для полного дуплекса (Intel SDM §13.4.34): CT=0x0F
    // (collision threshold), COLD=0x40 (collision distance).
    e1000_write(E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                                 (0x0Fu << E1000_TCTL_CT_SHIFT) |
                                 (0x40u << E1000_TCTL_COLD_SHIFT));
    // IPGT=10, IPGR1=8, IPGR2=6 — дефолт датащита для IEEE 802.3.
    e1000_write(E1000_REG_TIPG, 10u | (8u << 10) | (6u << 20));
    return 1;
}

/* ======================================================================== */
/* Публичное API                                                            */
/* ======================================================================== */

void e1000_init(void) {
    if (!e1000_find_controller()) return;
    if (!e1000_reset()) return;
    e1000_read_mac();
    if (!e1000_setup_rx()) return;
    if (!e1000_setup_tx()) return;

    e1000_write(E1000_REG_CTRL, e1000_read(E1000_REG_CTRL) | E1000_CTRL_SLU);

    e1000_found_flag = 1;

    DLOG("[E1000] MAC = %x:%x:%x:%x:%x:%x\n",
         e1000_mac[0], e1000_mac[1], e1000_mac[2], e1000_mac[3], e1000_mac[4], e1000_mac[5]);
    klog("[E1000] controller ready");
}

int e1000_found(void) {
    return e1000_found_flag;
}

const uint8_t *e1000_get_mac(void) {
    return e1000_mac;
}

int e1000_send(const void *frame, uint16_t len) {
    if (!e1000_found_flag || !frame || len == 0) return -1;
    if (len > PAGE_SIZE) return -1;

    uint64_t slot_flags = e1000_cli_save();
    uint32_t slot = e1000_tx_next;
    e1000_tx_next = (e1000_tx_next + 1) % E1000_TX_DESC_COUNT;
    e1000_sti_restore(slot_flags);

    // Ждём, пока слот освободится — при синхронной (stop-and-wait) модели
    // стека выше это обычно не ждёт вовсе.
    for (int i = 0; i < 2000 && !(e1000_tx_ring[slot].status & E1000_TX_STATUS_DD); i++) {
        pit_wait_ms(1);
    }
    if (!(e1000_tx_ring[slot].status & E1000_TX_STATUS_DD)) {
        DLOG("[E1000] TX slot %u still busy, dropping\n", slot);
        return -1;
    }

    memcpy(e1000_tx_buf_virt[slot], frame, len);

    e1000_tx_ring[slot].addr = e1000_tx_buf_phys[slot];
    e1000_tx_ring[slot].length = len;
    e1000_tx_ring[slot].cso = 0;
    e1000_tx_ring[slot].cmd = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    e1000_tx_ring[slot].status = 0;

    e1000_write(E1000_REG_TDT, (slot + 1) % E1000_TX_DESC_COUNT);

    for (int i = 0; i < 200; i++) {
        if (e1000_tx_ring[slot].status & E1000_TX_STATUS_DD) return 0;
        pit_wait_ms(1);
    }

    DLOG("[E1000] TX timed out\n");
    return -1;
}

void e1000_poll(void) {
    if (!e1000_found_flag) return;

    uint64_t busy_flags = e1000_cli_save();
    if (e1000_poll_busy) { e1000_sti_restore(busy_flags); return; }
    e1000_poll_busy = 1;
    e1000_sti_restore(busy_flags);

    uint32_t tail = e1000_rx_tail;
    for (int i = 0; i < E1000_POLL_MAX_PACKETS_PER_TICK; i++) {
        if (!(e1000_rx_ring[tail].status & E1000_RX_STATUS_DD)) break;

        uint16_t frame_len = e1000_rx_ring[tail].length; // SECRC уже срезал CRC
        if (frame_len > 0 && frame_len <= PAGE_SIZE) {
            eth_receive(e1000_rx_buf_virt[tail], frame_len);
        }

        e1000_rx_ring[tail].status = 0;
        e1000_write(E1000_REG_RDT, tail); // возвращаем дескриптор карте

        tail = (tail + 1) % E1000_RX_DESC_COUNT;
    }
    e1000_rx_tail = tail;

    e1000_read(E1000_REG_ICR); // подтверждаем всё, что успело защёлкнуться (IMS=0, см. заголовок файла)

    busy_flags = e1000_cli_save();
    e1000_poll_busy = 0;
    e1000_sti_restore(busy_flags);
}
