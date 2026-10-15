#include "e1000e.h"
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
/* Регистры — байт-в-байт та же карта смещений, что у e1000.c (см. его      */
/* заголовочный комментарий про общее происхождение 8254x/8257x).           */
/* ======================================================================== */

#define E1000E_REG_CTRL     0x0000
#define E1000E_REG_STATUS   0x0008
#define E1000E_REG_EERD     0x0014
#define E1000E_REG_ICR      0x00C0
#define E1000E_REG_IMS      0x00D0
#define E1000E_REG_IMC      0x00D8
#define E1000E_REG_RCTL     0x0100
#define E1000E_REG_TCTL     0x0400
#define E1000E_REG_TIPG     0x0410
#define E1000E_REG_RDBAL    0x2800
#define E1000E_REG_RDBAH    0x2804
#define E1000E_REG_RDLEN    0x2808
#define E1000E_REG_RDH      0x2810
#define E1000E_REG_RDT      0x2818
#define E1000E_REG_TDBAL    0x3800
#define E1000E_REG_TDBAH    0x3804
#define E1000E_REG_TDLEN    0x3808
#define E1000E_REG_TDH      0x3810
#define E1000E_REG_TDT      0x3818
#define E1000E_REG_RAL0     0x5400
#define E1000E_REG_RAH0     0x5404

#define E1000E_CTRL_SLU   (1u << 6)
#define E1000E_CTRL_RST   (1u << 26)

#define E1000E_EERD_START (1u << 0)
#define E1000E_EERD_DONE  (1u << 4)
#define E1000E_EERD_ADDR_SHIFT 8
#define E1000E_EERD_DATA_SHIFT 16

#define E1000E_RCTL_EN    (1u << 1)
#define E1000E_RCTL_BAM   (1u << 15)
#define E1000E_RCTL_SECRC (1u << 26)

#define E1000E_TCTL_EN    (1u << 1)
#define E1000E_TCTL_PSP   (1u << 3)
#define E1000E_TCTL_CT_SHIFT   4
#define E1000E_TCTL_COLD_SHIFT 12

#define E1000E_RAH_AV     (1u << 31)

#define E1000E_RX_STATUS_DD  (1u << 0)
#define E1000E_TX_CMD_EOP    (1u << 0)
#define E1000E_TX_CMD_IFCS   (1u << 1)
#define E1000E_TX_CMD_RS     (1u << 3)
#define E1000E_TX_STATUS_DD  (1u << 0)

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} e1000e_rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} e1000e_tx_desc_t;

#define E1000E_RX_DESC_COUNT 32
#define E1000E_TX_DESC_COUNT 8
#define E1000E_POLL_MAX_PACKETS_PER_TICK 8

/* ======================================================================== */
/* Состояние драйвера — та же cli/sti-guard дисциплина, что в rtl8139.c/    */
/* e1000.c (см. их комментарии у *_poll_busy).                              */
/* ======================================================================== */

static inline uint64_t e1000e_cli_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) :: "memory");
    __asm__ volatile ("cli");
    return flags;
}
static inline void e1000e_sti_restore(uint64_t flags) {
    __asm__ volatile ("push %0; popfq" : : "r"(flags) : "memory", "cc");
}
static volatile int e1000e_poll_busy = 0;

static int       e1000e_found_flag = 0;
static volatile uint8_t *e1000e_regs = NULL;
static uint8_t    e1000e_mac[6];

static volatile e1000e_rx_desc_t *e1000e_rx_ring = NULL;
static uint8_t    *e1000e_rx_buf_virt[E1000E_RX_DESC_COUNT];
static uint32_t    e1000e_rx_tail = 0;

static volatile e1000e_tx_desc_t *e1000e_tx_ring = NULL;
static uint8_t    *e1000e_tx_buf_virt[E1000E_TX_DESC_COUNT];
static uint64_t    e1000e_tx_buf_phys[E1000E_TX_DESC_COUNT];
static uint32_t    e1000e_tx_next = 0;

static inline uint32_t e1000e_read(uint32_t offset) {
    return *(volatile uint32_t *)(e1000e_regs + offset);
}
static inline void e1000e_write(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(e1000e_regs + offset) = value;
}

/* ======================================================================== */
/* PCI-обнаружение                                                          */
/* ======================================================================== */

// Не исчерпывающий список — см. тот же комментарий в e1000.c. Классическое
// 8254x-семейство (82540 и старше) сюда намеренно не входит — для него
// e1000.c.
static const uint16_t E1000E_DEVICE_IDS[] = {
    0x10D3, // 82574L (QEMU "e1000e")
    0x105E, // 82571EB
    0x10BC, // 82571EB (fiber/serdes)
    0x10D9, // 82571EB variant
    0x10F5, // 82567LM (ICH)
    0x1502, // 82579LM (ICH10/PCH)
    0x1503, // 82579V
};
#define E1000E_DEVICE_ID_COUNT (sizeof(E1000E_DEVICE_IDS) / sizeof(E1000E_DEVICE_IDS[0]))

static int e1000e_find_controller(void) {
    const pci_device_t *dev = NULL;
    for (uint32_t i = 0; i < E1000E_DEVICE_ID_COUNT && !dev; i++) {
        dev = pci_find_device(0x8086, E1000E_DEVICE_IDS[i]);
    }
    if (!dev) {
        DLOG("[E1000E] Controller not found\n");
        return 0;
    }

    DLOG("[E1000E] Found at %u:%u.%u (device id 0x%x)\n",
         dev->bus, dev->device, dev->function, dev->device_id);

    pci_bar_t bar0;
    if (pci_get_bar(dev, 0, &bar0) != 0 || bar0.is_io) {
        printf("[E1000E] Expected MMIO BAR0\n");
        return 0;
    }

    e1000e_regs = (volatile uint8_t *)mmio_map(bar0.address, bar0.size);
    if (!e1000e_regs) {
        printf("[E1000E] mmio_map failed\n");
        return 0;
    }
    DLOG("[E1000E] MMIO base = 0x%lx (phys 0x%lx, size 0x%lx)\n",
         (uint64_t)(uintptr_t)e1000e_regs, bar0.address, bar0.size);

    pci_enable_memory(dev);
    pci_enable_bus_master(dev);
    pci_disable_interrupts(dev);

    return 1;
}

/* ======================================================================== */
/* Сброс / EEPROM / настройка                                               */
/* ======================================================================== */

static int e1000e_reset(void) {
    e1000e_write(E1000E_REG_CTRL, e1000e_read(E1000E_REG_CTRL) | E1000E_CTRL_RST);

    for (int i = 0; i < 1000; i++) {
        if (!(e1000e_read(E1000E_REG_CTRL) & E1000E_CTRL_RST)) {
            DLOG("[E1000E] Reset complete\n");
            e1000e_write(E1000E_REG_IMC, 0xFFFFFFFFu);
            e1000e_read(E1000E_REG_ICR);
            return 1;
        }
        pit_wait_ms(1);
    }

    printf("[E1000E] Reset timed out\n");
    return 0;
}

static uint16_t e1000e_eeprom_read_word(uint8_t addr) {
    e1000e_write(E1000E_REG_EERD, ((uint32_t)addr << E1000E_EERD_ADDR_SHIFT) | E1000E_EERD_START);

    uint32_t val = 0;
    for (int i = 0; i < 100000; i++) {
        val = e1000e_read(E1000E_REG_EERD);
        if (val & E1000E_EERD_DONE) break;
    }
    return (uint16_t)(val >> E1000E_EERD_DATA_SHIFT);
}

// НАЙДЕННЫЙ БАГ (живое тестирование в QEMU: `-device e1000e` давал MAC
// "52:54:00:00:00:00" — верно только первые 2 байта, остальные 4 нулевые) —
// в отличие от классического 8254x (e1000.c, тот же приём там честно
// работает), NVM-эмуляция QEMU для 82574L не возвращает содержательные
// данные через EERD для слов 1 и 2 (слово 0 почему-то проходит, 1 и 2 —
// нет). Хотя по датащиту EERD — официальный задокументированный способ,
// на практике надёжнее то же, что делают настоящие e1000e-драйверы для
// новых чипов: RAL0/RAH0 аппаратно заполняются из NVM самим контроллером
// во время auto-read СРАЗУ после сброса (см. e1000e_reset()), раньше, чем
// сюда вообще попадает управление — читаем их НАПРЯМУЮ, никакого
// software-driven чтения EEPROM не нужно. EERD остаётся запасным путём на
// случай платформы, где RAH0.AV почему-то не взведён.
static void e1000e_read_mac(void) {
    uint32_t ral = e1000e_read(E1000E_REG_RAL0);
    uint32_t rah = e1000e_read(E1000E_REG_RAH0);

    if (rah & E1000E_RAH_AV) {
        e1000e_mac[0] = (uint8_t)(ral & 0xFF);
        e1000e_mac[1] = (uint8_t)((ral >> 8) & 0xFF);
        e1000e_mac[2] = (uint8_t)((ral >> 16) & 0xFF);
        e1000e_mac[3] = (uint8_t)((ral >> 24) & 0xFF);
        e1000e_mac[4] = (uint8_t)(rah & 0xFF);
        e1000e_mac[5] = (uint8_t)((rah >> 8) & 0xFF);
        return;
    }

    uint16_t w0 = e1000e_eeprom_read_word(0);
    uint16_t w1 = e1000e_eeprom_read_word(1);
    uint16_t w2 = e1000e_eeprom_read_word(2);

    e1000e_mac[0] = (uint8_t)(w0 & 0xFF);
    e1000e_mac[1] = (uint8_t)(w0 >> 8);
    e1000e_mac[2] = (uint8_t)(w1 & 0xFF);
    e1000e_mac[3] = (uint8_t)(w1 >> 8);
    e1000e_mac[4] = (uint8_t)(w2 & 0xFF);
    e1000e_mac[5] = (uint8_t)(w2 >> 8);

    uint32_t new_ral = ((uint32_t)e1000e_mac[3] << 24) | ((uint32_t)e1000e_mac[2] << 16) |
                        ((uint32_t)e1000e_mac[1] << 8) | (uint32_t)e1000e_mac[0];
    uint32_t new_rah = E1000E_RAH_AV | ((uint32_t)e1000e_mac[5] << 8) | (uint32_t)e1000e_mac[4];
    e1000e_write(E1000E_REG_RAL0, new_ral);
    e1000e_write(E1000E_REG_RAH0, new_rah);
}

static int e1000e_setup_rx(void) {
    uint64_t ring_phys = pmm_alloc_page();
    if (!ring_phys) { printf("[E1000E] RX ring alloc failed\n"); return 0; }
    e1000e_rx_ring = (volatile e1000e_rx_desc_t *)phys_to_virt(ring_phys);
    memset((void *)(uintptr_t)e1000e_rx_ring, 0, PAGE_SIZE);

    for (int i = 0; i < E1000E_RX_DESC_COUNT; i++) {
        uint64_t buf_phys = pmm_alloc_page();
        if (!buf_phys) { printf("[E1000E] RX buffer %d alloc failed\n", i); return 0; }
        e1000e_rx_buf_virt[i] = (uint8_t *)phys_to_virt(buf_phys);
        e1000e_rx_ring[i].addr = buf_phys;
        e1000e_rx_ring[i].status = 0;
    }
    e1000e_rx_tail = 0;

    e1000e_write(E1000E_REG_RDBAL, (uint32_t)ring_phys);
    e1000e_write(E1000E_REG_RDBAH, (uint32_t)(ring_phys >> 32));
    e1000e_write(E1000E_REG_RDLEN, E1000E_RX_DESC_COUNT * sizeof(e1000e_rx_desc_t));
    e1000e_write(E1000E_REG_RDH, 0);
    e1000e_write(E1000E_REG_RDT, E1000E_RX_DESC_COUNT - 1);

    e1000e_write(E1000E_REG_RCTL, E1000E_RCTL_EN | E1000E_RCTL_BAM | E1000E_RCTL_SECRC);
    return 1;
}

static int e1000e_setup_tx(void) {
    uint64_t ring_phys = pmm_alloc_page();
    if (!ring_phys) { printf("[E1000E] TX ring alloc failed\n"); return 0; }
    e1000e_tx_ring = (volatile e1000e_tx_desc_t *)phys_to_virt(ring_phys);
    memset((void *)(uintptr_t)e1000e_tx_ring, 0, PAGE_SIZE);

    for (int i = 0; i < E1000E_TX_DESC_COUNT; i++) {
        uint64_t buf_phys = pmm_alloc_page();
        if (!buf_phys) { printf("[E1000E] TX buffer %d alloc failed\n", i); return 0; }
        e1000e_tx_buf_phys[i] = buf_phys;
        e1000e_tx_buf_virt[i] = (uint8_t *)phys_to_virt(buf_phys);
        e1000e_tx_ring[i].status = E1000E_TX_STATUS_DD;
    }
    e1000e_tx_next = 0;

    e1000e_write(E1000E_REG_TDBAL, (uint32_t)ring_phys);
    e1000e_write(E1000E_REG_TDBAH, (uint32_t)(ring_phys >> 32));
    e1000e_write(E1000E_REG_TDLEN, E1000E_TX_DESC_COUNT * sizeof(e1000e_tx_desc_t));
    e1000e_write(E1000E_REG_TDH, 0);
    e1000e_write(E1000E_REG_TDT, 0);

    e1000e_write(E1000E_REG_TCTL, E1000E_TCTL_EN | E1000E_TCTL_PSP |
                                   (0x0Fu << E1000E_TCTL_CT_SHIFT) |
                                   (0x40u << E1000E_TCTL_COLD_SHIFT));
    e1000e_write(E1000E_REG_TIPG, 10u | (8u << 10) | (6u << 20));
    return 1;
}

/* ======================================================================== */
/* Публичное API                                                            */
/* ======================================================================== */

void e1000e_init(void) {
    if (!e1000e_find_controller()) return;
    if (!e1000e_reset()) return;
    e1000e_read_mac();
    if (!e1000e_setup_rx()) return;
    if (!e1000e_setup_tx()) return;

    e1000e_write(E1000E_REG_CTRL, e1000e_read(E1000E_REG_CTRL) | E1000E_CTRL_SLU);

    e1000e_found_flag = 1;

    DLOG("[E1000E] MAC = %x:%x:%x:%x:%x:%x\n",
         e1000e_mac[0], e1000e_mac[1], e1000e_mac[2], e1000e_mac[3], e1000e_mac[4], e1000e_mac[5]);
    klog("[E1000E] controller ready");
}

int e1000e_found(void) {
    return e1000e_found_flag;
}

const uint8_t *e1000e_get_mac(void) {
    return e1000e_mac;
}

int e1000e_send(const void *frame, uint16_t len) {
    if (!e1000e_found_flag || !frame || len == 0) return -1;
    if (len > PAGE_SIZE) return -1;

    uint64_t slot_flags = e1000e_cli_save();
    uint32_t slot = e1000e_tx_next;
    e1000e_tx_next = (e1000e_tx_next + 1) % E1000E_TX_DESC_COUNT;
    e1000e_sti_restore(slot_flags);

    for (int i = 0; i < 2000 && !(e1000e_tx_ring[slot].status & E1000E_TX_STATUS_DD); i++) {
        pit_wait_ms(1);
    }
    if (!(e1000e_tx_ring[slot].status & E1000E_TX_STATUS_DD)) {
        DLOG("[E1000E] TX slot %u still busy, dropping\n", slot);
        return -1;
    }

    memcpy(e1000e_tx_buf_virt[slot], frame, len);

    e1000e_tx_ring[slot].addr = e1000e_tx_buf_phys[slot];
    e1000e_tx_ring[slot].length = len;
    e1000e_tx_ring[slot].cso = 0;
    e1000e_tx_ring[slot].cmd = E1000E_TX_CMD_EOP | E1000E_TX_CMD_IFCS | E1000E_TX_CMD_RS;
    e1000e_tx_ring[slot].status = 0;

    e1000e_write(E1000E_REG_TDT, (slot + 1) % E1000E_TX_DESC_COUNT);

    for (int i = 0; i < 200; i++) {
        if (e1000e_tx_ring[slot].status & E1000E_TX_STATUS_DD) return 0;
        pit_wait_ms(1);
    }

    DLOG("[E1000E] TX timed out\n");
    return -1;
}

void e1000e_poll(void) {
    if (!e1000e_found_flag) return;

    uint64_t busy_flags = e1000e_cli_save();
    if (e1000e_poll_busy) { e1000e_sti_restore(busy_flags); return; }
    e1000e_poll_busy = 1;
    e1000e_sti_restore(busy_flags);

    uint32_t tail = e1000e_rx_tail;
    for (int i = 0; i < E1000E_POLL_MAX_PACKETS_PER_TICK; i++) {
        if (!(e1000e_rx_ring[tail].status & E1000E_RX_STATUS_DD)) break;

        uint16_t frame_len = e1000e_rx_ring[tail].length;
        if (frame_len > 0 && frame_len <= PAGE_SIZE) {
            eth_receive(e1000e_rx_buf_virt[tail], frame_len);
        }

        e1000e_rx_ring[tail].status = 0;
        e1000e_write(E1000E_REG_RDT, tail);

        tail = (tail + 1) % E1000E_RX_DESC_COUNT;
    }
    e1000e_rx_tail = tail;

    e1000e_read(E1000E_REG_ICR);

    busy_flags = e1000e_cli_save();
    e1000e_poll_busy = 0;
    e1000e_sti_restore(busy_flags);
}
