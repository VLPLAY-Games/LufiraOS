#include "xhci.h"
#include "usb.h"
#include "usb_hid.h"

#include "drivers/pci/pci.h"
#include "system/mm/pmm.h"
#include "system/mm/paging.h"
#include "system/timer/pit.h"
#include "drivers/console/console.h"
#include "lib/string.h"
#include "system/devmode/devmode.h"
#include "system/klog/klog.h"

/*
 * xHCI-драйвер: контроллер + boot-протокол HID (клавиатура/мышь).
 * Заменяет прежний UHCI-драйвер — общая архитектура (PCI-обнаружение,
 * сброс, синхронная энумерация через control-передачи, boot-протокол HID,
 * опрос из usb_poll() раз в тик PIT вместо настоящих IRQ) сознательно
 * повторяет uhci.c, но внутренний механизм принципиально другой: вместо
 * Frame List из Queue Head/TD — Command Ring, Event Ring и per-устройство
 * Device/Input Context, общие для всех операций контроллера.
 *
 * Прерывания контроллера НЕ используются (как и у UHCI/AC'97) — ни
 * APIC/MSI, ни даже legacy PCI INTx: в этом ядре нет ни MSI-X, ни разбора
 * PCI capability list вообще, а добавлять их ради опроса, который и так
 * дёшев (проверка Cycle-бита), смысла не имеет. Event Ring опрашивается
 * напрямую из usb_poll().
 */

/* ======================================================================== */
/* MMIO-доступ и отображение BAR0                                           */
/* ======================================================================== */

static inline uint32_t reg_read32(void *base, uint32_t off) {
    return *(volatile uint32_t *)((uint8_t *)base + off);
}
static inline void reg_write32(void *base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)((uint8_t *)base + off) = val;
}
static inline uint64_t reg_read64(void *base, uint32_t off) {
    return *(volatile uint64_t *)((uint8_t *)base + off);
}
static inline void reg_write64(void *base, uint32_t off, uint64_t val) {
    *(volatile uint64_t *)((uint8_t *)base + off) = val;
}

// Бамп-аллокатор поверх KERNEL_MMIO_BASE — единственный потребитель этого
// диапазона здесь один (BAR0 xHCI), поэтому полноценный менеджер MMIO-окон
// был бы избыточен.
static uint64_t xhci_mmio_next_free = KERNEL_MMIO_BASE;

static void *map_mmio(uint64_t phys, uint64_t size) {
    if (size == 0) size = 0x2000; // защитный дефолт на случай нулевого BAR.size

    uint64_t phys_page = phys & ~((uint64_t)PAGE_SIZE - 1);
    uint64_t phys_offset = phys - phys_page;
    uint64_t map_size = (phys_offset + size + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1);

    uint64_t virt_base = xhci_mmio_next_free;
    xhci_mmio_next_free += map_size;

    for (uint64_t off = 0; off < map_size; off += PAGE_SIZE) {
        if (map_page(virt_base + off, phys_page + off,
                      PAGE_PRESENT | PAGE_WRITE | PAGE_PCD) != 0)
        {
            printf("[XHCI] map_mmio: map_page failed at phys 0x%lx\n", phys_page + off);
            return NULL;
        }
    }
    return (void *)(uintptr_t)(virt_base + phys_offset);
}

/* ======================================================================== */
/* Регистры xHCI                                                            */
/* ======================================================================== */

/* Capability Registers (смещения от cap_base = MMIO BAR0) */
#define XHCI_CAP_CAPLENGTH  0x00 /* u8  */
#define XHCI_CAP_HCSPARAMS1 0x04 /* u32 */
#define XHCI_CAP_HCSPARAMS2 0x08 /* u32 */
#define XHCI_CAP_HCCPARAMS1 0x10 /* u32 */
#define XHCI_CAP_DBOFF      0x14 /* u32 */
#define XHCI_CAP_RTSOFF     0x18 /* u32 */

#define XHCI_HCSPARAMS1_MAXSLOTS(v) ((v) & 0xFFu)
#define XHCI_HCSPARAMS1_MAXPORTS(v) (((v) >> 24) & 0xFFu)
#define XHCI_HCSPARAMS2_MAX_SCRATCHPAD(v) \
    (((((v) >> 21) & 0x1Fu) << 5) | (((v) >> 27) & 0x1Fu))
#define XHCI_HCCPARAMS1_CSZ(v) (((v) >> 2) & 0x1u)

/* Operational Registers (смещения от op_base = cap_base + CAPLENGTH) */
#define XHCI_OP_USBCMD 0x00 /* u32 */
#define XHCI_OP_USBSTS 0x04 /* u32 */
#define XHCI_OP_PAGESIZE 0x08 /* u32 */
#define XHCI_OP_CRCR   0x18 /* u64 */
#define XHCI_OP_DCBAAP 0x30 /* u64 */
#define XHCI_OP_CONFIG 0x38 /* u32 */
#define XHCI_OP_PORTSC(n) (0x400u + ((uint32_t)(n) - 1u) * 0x10u) /* u32, n = 1..MaxPorts */

#define XHCI_USBCMD_RS     (1u << 0)
#define XHCI_USBCMD_HCRST  (1u << 1)

#define XHCI_USBSTS_HCH (1u << 0)
#define XHCI_USBSTS_CNR (1u << 11)

#define XHCI_CRCR_RCS (1ull << 0)

#define XHCI_PORTSC_CCS (1u << 0)
#define XHCI_PORTSC_PED (1u << 1)
#define XHCI_PORTSC_PR  (1u << 4)
#define XHCI_PORTSC_PP  (1u << 9)
#define XHCI_PORTSC_SPEED(v) (((v) >> 10) & 0xFu)
#define XHCI_PORTSC_CSC (1u << 17)
#define XHCI_PORTSC_PEC (1u << 18)
#define XHCI_PORTSC_WRC (1u << 19)
#define XHCI_PORTSC_OCC (1u << 20)
#define XHCI_PORTSC_PRC (1u << 21)
#define XHCI_PORTSC_PLC (1u << 22)
#define XHCI_PORTSC_CEC (1u << 23)
// Биты "запись 1 сбрасывает" — маскируем ПЕРЕД любым read-modify-write,
// иначе можно случайно затереть ещё не обработанное событие (тот же
// принцип, что и UHCI_PORTSC_RWC_MASK в прежнем uhci.c).
#define XHCI_PORTSC_RW1C_MASK \
    (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC | \
     XHCI_PORTSC_OCC | XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | XHCI_PORTSC_CEC)

#define XHCI_SPEED_FULL  1
#define XHCI_SPEED_LOW   2
#define XHCI_SPEED_HIGH  3
#define XHCI_SPEED_SUPER 4

/* Runtime Registers (смещения от rt_base = cap_base + (RTSOFF & ~0x1F)) */
#define XHCI_RT_IR0 0x20 // Interrupter 0 — единственный, который мы используем

#define XHCI_IR_ERSTSZ 0x08 /* u32 */
#define XHCI_IR_ERSTBA 0x10 /* u64 */
#define XHCI_IR_ERDP   0x18 /* u64 */

/* Doorbell Array (смещения от db_base = cap_base + (DBOFF & ~0x3)) */
#define XHCI_DCI_CONTROL 1 // DCI управляющего endpoint'а (EP0) у любого слота

/* TRB types (control биты 10-15) */
#define TRB_TYPE_NORMAL                   1
#define TRB_TYPE_SETUP_STAGE               2
#define TRB_TYPE_DATA_STAGE                3
#define TRB_TYPE_STATUS_STAGE              4
#define TRB_TYPE_LINK                      6
#define TRB_TYPE_ENABLE_SLOT_CMD           9
#define TRB_TYPE_ADDRESS_DEVICE_CMD        11
#define TRB_TYPE_CONFIGURE_ENDPOINT_CMD    12
#define TRB_TYPE_EVALUATE_CONTEXT_CMD      13
#define TRB_TYPE_TRANSFER_EVENT            32
#define TRB_TYPE_COMMAND_COMPLETION_EVENT  33

#define TRB_CONTROL_CYCLE      (1u << 0)
#define TRB_CONTROL_TC         (1u << 1)  // Link TRB: Toggle Cycle
#define TRB_CONTROL_IOC        (1u << 5)  // Interrupt On Completion
#define TRB_CONTROL_IDT        (1u << 6)  // Setup Stage: setup-пакет инлайн в parameter
#define TRB_CONTROL_TYPE_SHIFT 10
#define TRB_CONTROL_TYPE_SET(t) (((uint32_t)(t)) << TRB_CONTROL_TYPE_SHIFT)
#define TRB_CONTROL_TYPE_GET(c) (((c) >> TRB_CONTROL_TYPE_SHIFT) & 0x3Fu)
#define TRB_CONTROL_SLOT_SHIFT 24
#define TRB_CONTROL_EP_DIR_IN  (1u << 16) // Data/Status Stage: направление

#define TRB_TRT_SHIFT   16
#define TRB_TRT_NO_DATA 0
#define TRB_TRT_OUT     2
#define TRB_TRT_IN      3

#define TRB_COMPLETION_CODE_SHIFT 24
#define TRB_COMPLETION_SUCCESS    1

/* ======================================================================== */
/* TRB и кольца (Command Ring / Event Ring / Transfer Ring)                 */
/* ======================================================================== */

typedef struct __attribute__((packed, aligned(16))) {
    volatile uint64_t parameter;
    volatile uint32_t status;
    volatile uint32_t control;
} xhci_trb_t;

// Одна страница (4096 байт = 256 TRB) на кольцо. У командных/transfer-колец
// последний слот навсегда занят Link TRB (замыкает кольцо на начало) — 255
// используемых слотов. Event Ring использует все 256 напрямую (перенос на
// начало определяется по перевороту ожидаемого Cycle-бита, без Link TRB).
// Каждая xHCI-структура в этом драйвере укладывается в ОДНУ страницу —
// pmm_alloc_page() выделяет ровно одну 4KB-страницу без гарантии
// физической непрерывности между разными вызовами, так что многостраничные
// структуры тут принципиально не используются.
#define XHCI_RING_TRB_CAPACITY 256
#define XHCI_RING_USABLE_TRBS  255

typedef struct {
    xhci_trb_t *trbs;
    uint64_t    phys;
    uint32_t    enqueue_index;
    uint32_t    dequeue_index;
    uint8_t     cycle_state;
    uint8_t     is_event_ring;
} xhci_ring_t;

typedef struct __attribute__((packed)) {
    uint64_t ring_segment_base;
    uint32_t ring_segment_size; // биты 0-15 = размер сегмента в TRB
    uint32_t reserved;
} xhci_erst_entry_t;

static int xhci_ring_init(xhci_ring_t *ring, int is_event_ring) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return -1;

    ring->trbs = (xhci_trb_t *)phys_to_virt(phys);
    ring->phys = phys;
    memset(ring->trbs, 0, PAGE_SIZE);
    ring->enqueue_index = 0;
    ring->dequeue_index = 0;
    ring->cycle_state = 1;
    ring->is_event_ring = (uint8_t)is_event_ring;

    if (!is_event_ring) {
        xhci_trb_t *link = &ring->trbs[XHCI_RING_USABLE_TRBS];
        link->parameter = ring->phys;
        link->status = 0;
        link->control = TRB_CONTROL_TYPE_SET(TRB_TYPE_LINK) | TRB_CONTROL_TC | TRB_CONTROL_CYCLE;
    }
    return 0;
}

// Записывает TRB на текущей позиции enqueue (Cycle-бит подставляется
// автоматически из ring->cycle_state), продвигает индекс (перепрыгивая
// через постоянный Link TRB и переворачивая cycle_state при заворачивании),
// возвращает указатель на только что записанный TRB — вызывающий сам
// переводит его в физический адрес для сопоставления с Event TRB.
static xhci_trb_t *xhci_ring_enqueue(xhci_ring_t *ring, uint64_t parameter,
                                      uint32_t status, uint32_t control_no_cycle)
{
    xhci_trb_t *trb = &ring->trbs[ring->enqueue_index];
    trb->parameter = parameter;
    trb->status = status;
    __asm__ volatile ("" ::: "memory"); // parameter/status видны раньше Cycle-бита
    trb->control = control_no_cycle | ((uint32_t)ring->cycle_state & TRB_CONTROL_CYCLE);

    ring->enqueue_index++;
    if (ring->enqueue_index >= XHCI_RING_USABLE_TRBS) {
        ring->enqueue_index = 0;
        // Постоянный Link TRB был инициализирован с Cycle=1 один раз в
        // xhci_ring_init() и с тех пор никогда не обновлялся — на первом
        // обороте кольца это ещё случайно совпадало с ring->cycle_state
        // (тоже 1), но начиная со ВТОРОГО оборота (cycle_state уже 0)
        // контроллер видел на месте Link TRB "старый" Cycle-бит,
        // расценивал его как непроизведённый (T=1 не помогает — Toggle
        // Cycle указывает, что делать ПОСЛЕ перехода по Link TRB, а не
        // разрешает саму валидность самого Link TRB) и просто зависал на
        // границе круга, не переходя обратно в начало кольца. Именно
        // это (а не только короткий таймаут) было настоящей причиной
        // регулярных сбоев mount на больших объёмах чтения — сбой
        // стабильно происходил в районе кратных ~255 передач через одно
        // и то же кольцо. Исправление: обновлять Cycle-бит Link TRB
        // каждый раз при заворачивании кольца, синхронно с
        // ring->cycle_state.
        xhci_trb_t *link = &ring->trbs[XHCI_RING_USABLE_TRBS];
        link->control = TRB_CONTROL_TYPE_SET(TRB_TYPE_LINK) | TRB_CONTROL_TC
            | ((uint32_t)ring->cycle_state & TRB_CONTROL_CYCLE);
        ring->cycle_state ^= 1;
    }
    return trb;
}

static inline uint64_t xhci_trb_phys(const xhci_ring_t *ring, const xhci_trb_t *trb) {
    return ring->phys + (uint64_t)(trb - ring->trbs) * sizeof(xhci_trb_t);
}

/* ======================================================================== */
/* Slot/Endpoint/Input Control Context (32 или 64 байта — см. CSZ)          */
/* ======================================================================== */

static uint32_t xhci_context_size = 32; // выставляется из HCCPARAMS1.CSZ при инициализации

// index 0 = Input Control Context (только в Input Context) или Slot Context
// (в Device Context); index 1 = Slot Context (в Input Context); index
// (1+dci) = Endpoint Context для DCI dci (1..31).
static inline uint32_t *xhci_ctx_at(void *base, int index) {
    return (uint32_t *)((uint8_t *)base + (uint64_t)index * xhci_context_size);
}

#define SLOT_DW0_SPEED_SHIFT       20
#define SLOT_DW0_CTX_ENTRIES_SHIFT 27
#define SLOT_DW1_ROOT_PORT_SHIFT   16
#define SLOT_DW3_USB_ADDR(v)       ((v) & 0xFFu)

#define EP_DW0_INTERVAL_SHIFT   16
#define EP_DW1_CERR_SHIFT       1
#define EP_DW1_TYPE_SHIFT       3
#define EP_DW1_MAXPACKET_SHIFT  16
#define EP_TYPE_BULK_OUT      2
#define EP_TYPE_CONTROL       4
#define EP_TYPE_BULK_IN       6
#define EP_TYPE_INTERRUPT_IN  7

#define ICC_DW1_ADD_FLAG(dci) (1u << (dci)) // dci==0 означает Slot Context (A0)

static void xhci_fill_slot_ctx(uint32_t *slot_ctx, uint8_t speed, uint8_t port,
                                uint8_t context_entries)
{
    slot_ctx[0] = ((uint32_t)speed << SLOT_DW0_SPEED_SHIFT)
                | ((uint32_t)context_entries << SLOT_DW0_CTX_ENTRIES_SHIFT);
    slot_ctx[1] = (uint32_t)port << SLOT_DW1_ROOT_PORT_SHIFT;
    slot_ctx[2] = 0;
    slot_ctx[3] = 0;
}

static void xhci_fill_ep_ctx(uint32_t *ep_ctx, uint8_t ep_type, uint16_t max_packet,
                              uint8_t interval, uint64_t ring_phys, uint8_t dcs)
{
    ep_ctx[0] = (uint32_t)interval << EP_DW0_INTERVAL_SHIFT;
    ep_ctx[1] = (3u << EP_DW1_CERR_SHIFT) | ((uint32_t)ep_type << EP_DW1_TYPE_SHIFT)
              | ((uint32_t)max_packet << EP_DW1_MAXPACKET_SHIFT);
    ep_ctx[2] = (uint32_t)(ring_phys & 0xFFFFFFF0u) | ((uint32_t)dcs & 1u);
    ep_ctx[3] = (uint32_t)(ring_phys >> 32);
    ep_ctx[4] = 8u; // Average TRB Length — с запасом для setup/interrupt-отчётов
}

// LS/FS: bInterval — число 1мс-кадров (обычно 8 или 10 у boot-HID);
// переводим в единицы по 125мкс (*8) и берём индекс старшего установленного
// бита. HS/SS: bInterval уже задаёт log2 напрямую (Interval = bInterval-1).
// Точная синхронизация тут не критична — клавиатура/мышь и так опрашиваются
// не чаще раза в тик PIT (10мс), см. usb_poll().
static uint8_t xhci_compute_interval(uint8_t speed, uint8_t bInterval) {
    if (speed == XHCI_SPEED_HIGH || speed == XHCI_SPEED_SUPER) {
        uint8_t v = bInterval ? bInterval : 1;
        if (v > 16) v = 16;
        return (uint8_t)(v - 1);
    }
    uint32_t units = (uint32_t)(bInterval ? bInterval : 1) * 8u;
    uint8_t idx = 0;
    while ((units >>= 1) != 0) idx++;
    if (idx < 3) idx = 3;
    if (idx > 10) idx = 10;
    return idx;
}

static uint16_t xhci_default_ep0_max_packet(uint8_t speed) {
    switch (speed) {
        case XHCI_SPEED_LOW:   return 8;
        case XHCI_SPEED_HIGH:  return 64;
        case XHCI_SPEED_SUPER: return 512;
        case XHCI_SPEED_FULL:
        default:                return 8;
    }
}

/* ======================================================================== */
/* Состояние драйвера                                                       */
/* ======================================================================== */

#define XHCI_MAX_SLOTS_SUPPORTED 8 // пишется в CONFIG.MaxSlotsEn
#define XHCI_MAX_HID_DEVICES     8
#define XHCI_MAX_MSD_DEVICES     4
#define XHCI_DMA_DATA_MAX        512 // с запасом под конфигурацию с HID/endpoint-дескрипторами

#define XHCI_DEV_CLASS_NONE 0
#define XHCI_DEV_CLASS_HID  1
#define XHCI_DEV_CLASS_MSD  2

typedef struct {
    int      in_use;
    uint8_t  slot_id, port_id, speed;
    int      device_class; // XHCI_DEV_CLASS_*

    uint64_t device_ctx_phys; void *device_ctx_virt;
    uint64_t input_ctx_phys;  void *input_ctx_virt;

    uint16_t    ep0_max_packet;
    xhci_ring_t ep0_ring;

    // HID: не больше одного interrupt IN endpoint'а на слот.
    int         hid_ep_dci;
    xhci_ring_t hid_ep_ring;
    uint64_t    hid_report_buf_phys; void *hid_report_buf_virt;
    uint8_t     hid_report_expected_len, hid_protocol;

    // Mass Storage: пара bulk-endpoint'ов (IN + OUT) для Bulk-Only Transport.
    int         msd_bulk_in_dci, msd_bulk_out_dci;
    xhci_ring_t msd_bulk_in_ring, msd_bulk_out_ring;
} xhci_slot_t;

static void *xhci_cap_base = NULL;
static void *xhci_op_base = NULL;
static void *xhci_rt_base = NULL;
static void *xhci_db_base = NULL;
static void *xhci_ir0_base = NULL;

static const pci_device_t *xhci_dev = NULL;

static uint32_t xhci_max_slots = 0;
static uint32_t xhci_max_ports = 0;

static uint64_t  xhci_dcbaa_phys = 0;
static uint64_t *xhci_dcbaa_virt = NULL;

static xhci_ring_t xhci_cmd_ring;
static xhci_ring_t xhci_event_ring;
static uint64_t    xhci_erst_phys = 0;

static uint64_t xhci_dma_scratch_phys = 0;
static uint8_t *xhci_dma_scratch_virt = NULL;

static xhci_slot_t xhci_slots[XHCI_MAX_SLOTS_SUPPORTED];
static xhci_hid_device_t xhci_hid_devices[XHCI_MAX_HID_DEVICES];
static int xhci_hid_device_count = 0;

typedef struct {
    int      in_use;
    uint8_t  slot_id;
    uint32_t max_lba;
    uint32_t block_size;
} xhci_msd_device_t;

static xhci_msd_device_t xhci_msd_devices[XHCI_MAX_MSD_DEVICES];
static int xhci_msd_device_count_var = 0;
static uint32_t xhci_msd_next_tag = 1;

// true, когда хотя бы одно HID-устройство вооружено и usb_poll() должно
// реально что-то опрашивать.
static int xhci_ready = 0;

static xhci_slot_t *xhci_slot_for_id(uint8_t slot_id) {
    if (slot_id == 0 || slot_id > XHCI_MAX_SLOTS_SUPPORTED) return NULL;
    xhci_slot_t *s = &xhci_slots[slot_id - 1];
    return s->in_use ? s : NULL;
}

static void xhci_ring_doorbell(uint8_t slot_id, uint8_t target) {
    reg_write32(xhci_db_base, (uint32_t)slot_id * 4u, target);
}

/* ======================================================================== */
/* PCI-обнаружение                                                          */
/* ======================================================================== */

static int xhci_find_controller(void) {
    // class 0x0C = Serial Bus Controller, subclass 0x03 = USB, prog-if 0x30 = xHCI
    const pci_device_t *dev = pci_find_class_if(0x0C, 0x03, 0x30);
    if (!dev) {
        printf("[XHCI] Controller not found\n");
        return 0;
    }

    DLOG("[XHCI] Controller found at %u:%u.%u (vendor=%04X device=%04X)\n",
         dev->bus, dev->device, dev->function, dev->vendor_id, dev->device_id);

    pci_bar_t bar0;
    if (pci_get_bar(dev, 0, &bar0) != 0) {
        printf("[XHCI] Failed to read BAR0\n");
        return 0;
    }
    if (bar0.is_io) {
        printf("[XHCI] Expected a memory BAR at BAR0\n");
        return 0;
    }

    xhci_dev = dev;

    pci_enable_memory(dev);
    pci_enable_bus_master(dev);
    // Опрашиваем сами (как AC'97 и прежний UHCI) — PCI IRQ не используем.
    pci_disable_interrupts(dev);

    xhci_cap_base = map_mmio(bar0.address, bar0.size);
    if (!xhci_cap_base) {
        printf("[XHCI] Failed to map MMIO BAR0\n");
        return 0;
    }

    DLOG("[XHCI] BAR0 phys=0x%lx size=0x%lx mapped at %p\n",
         bar0.address, bar0.size, xhci_cap_base);

    return 1;
}

/* ======================================================================== */
/* Сброс контроллера                                                        */
/* ======================================================================== */

static int xhci_reset(void) {
    uint32_t cmd = reg_read32(xhci_op_base, XHCI_OP_USBCMD);
    if (cmd & XHCI_USBCMD_RS) {
        reg_write32(xhci_op_base, XHCI_OP_USBCMD, cmd & ~XHCI_USBCMD_RS);
        int timeout = 500;
        while (timeout-- > 0 && !(reg_read32(xhci_op_base, XHCI_OP_USBSTS) & XHCI_USBSTS_HCH)) {
            pit_wait_ms(1);
        }
    }

    reg_write32(xhci_op_base, XHCI_OP_USBCMD, XHCI_USBCMD_HCRST);

    int timeout = 1000; // с запасом — на реальном железе сброс может занять больше 100мс
    while (timeout-- > 0) {
        pit_wait_ms(1);
        if (!(reg_read32(xhci_op_base, XHCI_OP_USBCMD) & XHCI_USBCMD_HCRST) &&
            !(reg_read32(xhci_op_base, XHCI_OP_USBSTS) & XHCI_USBSTS_CNR))
            break;
    }

    if ((reg_read32(xhci_op_base, XHCI_OP_USBCMD) & XHCI_USBCMD_HCRST) ||
        (reg_read32(xhci_op_base, XHCI_OP_USBSTS) & XHCI_USBSTS_CNR))
    {
        printf("[XHCI] WARNING: Host Controller Reset did not complete\n");
        return 0;
    }
    return 1;
}

/* ======================================================================== */
/* DCBAA + Scratchpad Buffer Array                                          */
/* ======================================================================== */

static int xhci_setup_dcbaa(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) { printf("[XHCI] Out of memory for DCBAA\n"); return 0; }

    xhci_dcbaa_phys = phys;
    xhci_dcbaa_virt = (uint64_t *)phys_to_virt(phys);
    memset(xhci_dcbaa_virt, 0, PAGE_SIZE);

    reg_write64(xhci_op_base, XHCI_OP_DCBAAP, xhci_dcbaa_phys);
    return 1;
}

// DCBAA[0] — не слот устройства, а специальный указатель на Scratchpad
// Buffer Array, если HCSPARAMS2 сообщает ненулевое число scratchpad-буферов
// (нужны контроллеру для собственных внутренних нужд — некоторые реализации
// отказываются выходить из состояния Halted без них). Пропускается, если
// число равно 0 (обычно так и есть в QEMU).
static int xhci_setup_scratchpad(uint32_t hcsparams2) {
    uint32_t count = XHCI_HCSPARAMS2_MAX_SCRATCHPAD(hcsparams2);
    if (count == 0) return 1;

    uint64_t array_phys = pmm_alloc_page();
    if (!array_phys) { printf("[XHCI] Out of memory for scratchpad array\n"); return 0; }
    uint64_t *array_virt = (uint64_t *)phys_to_virt(array_phys);
    memset(array_virt, 0, PAGE_SIZE);

    uint32_t max_entries = PAGE_SIZE / sizeof(uint64_t);
    if (count > max_entries) count = max_entries;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t buf_phys = pmm_alloc_page();
        if (!buf_phys) { printf("[XHCI] Out of memory for scratchpad buffer %u\n", i); return 0; }
        array_virt[i] = buf_phys;
    }

    xhci_dcbaa_virt[0] = array_phys;
    return 1;
}

/* ======================================================================== */
/* Event Ring + ERST, ожидание событий                                      */
/* ======================================================================== */

static int xhci_setup_event_ring(void) {
    if (xhci_ring_init(&xhci_event_ring, 1) != 0) {
        printf("[XHCI] Out of memory for Event Ring\n");
        return 0;
    }

    uint64_t erst_phys = pmm_alloc_page();
    if (!erst_phys) { printf("[XHCI] Out of memory for ERST\n"); return 0; }
    xhci_erst_phys = erst_phys;
    xhci_erst_entry_t *erst = (xhci_erst_entry_t *)phys_to_virt(erst_phys);
    memset(erst, 0, PAGE_SIZE);
    erst->ring_segment_base = xhci_event_ring.phys;
    erst->ring_segment_size = XHCI_RING_TRB_CAPACITY;
    erst->reserved = 0;

    reg_write32(xhci_ir0_base, XHCI_IR_ERSTSZ, 1);
    reg_write64(xhci_ir0_base, XHCI_IR_ERSTBA, xhci_erst_phys);
    reg_write64(xhci_ir0_base, XHCI_IR_ERDP, xhci_event_ring.phys); // EHB=0, DESI=0

    return 1;
}

// TRB выровнены на 16 байт, так что их физический адрес уже имеет 4 младших
// бита нулевыми — запись его напрямую в ERDP автоматически даёт DESI=0 и
// EHB=0 (один сегмент, событие уже обработано).
static void xhci_write_erdp(void) {
    uint64_t ptr = xhci_event_ring.phys +
        (uint64_t)xhci_event_ring.dequeue_index * sizeof(xhci_trb_t);
    reg_write64(xhci_ir0_base, XHCI_IR_ERDP, ptr);
}

// Декодирует один завершённый interrupt IN HID-отчёт и НЕМЕДЛЕННО
// перевооружает конвейер (та же логика, что раньше жила только в
// usb_poll()). Вынесено в общий хелпер по важной причине: Event Ring один
// на все endpoint'ы разом, и синхронное ожидание MSD/control-передачи
// (xhci_wait_for_event с ненулевым want_ptr) неизбежно попутно вычитывает
// из очереди и чужие события — например, ровно те же interrupt IN отчёты
// клавиатуры/мыши, которые иначе обслуживал бы только usb_poll(). Если
// такое событие просто отбросить (как раньше), клавиатура/мышь навсегда
// остаётся без перевооружённого TD — их endpoint "молчит" до перезагрузки.
// Обслуживая HID-событие ПРЯМО ТУТ, независимо от того, кто именно сейчас
// дренирует кольцо, конвейер клавиатуры/мыши никогда не голодает.
static void xhci_service_hid_event(uint8_t slot_id, uint32_t status) {
    xhci_slot_t *slot = xhci_slot_for_id(slot_id);
    if (!slot || !slot->hid_ep_dci) return; // не HID-событие (например, MSD) — не наше дело

    uint8_t cc = (uint8_t)((status >> TRB_COMPLETION_CODE_SHIFT) & 0xFFu);
    if (cc == TRB_COMPLETION_SUCCESS) {
        if (slot->hid_protocol == USB_HID_PROTOCOL_KEYBOARD) {
            usb_hid_keyboard_report((const uint8_t *)slot->hid_report_buf_virt);
        } else {
            usb_hid_mouse_report((const uint8_t *)slot->hid_report_buf_virt,
                                  slot->hid_report_expected_len);
        }
    }

    xhci_ring_enqueue(&slot->hid_ep_ring, slot->hid_report_buf_phys,
                       slot->hid_report_expected_len,
                       TRB_CONTROL_TYPE_SET(TRB_TYPE_NORMAL) | TRB_CONTROL_IOC);
    xhci_ring_doorbell(slot->slot_id, (uint8_t)slot->hid_ep_dci);
}

// Один общий Event Ring и один Interrupter обслуживают все запросы —
// синхронное ожидание конкретного события (энумерация, control- и
// bulk-передачи, want_ptr != 0, с таймаутом) и опрос usb_poll()
// (want_ptr == 0, без блокировки). Это НЕ два непересекающихся во времени
// сценария (в отличие от изначального предположения на этапе проектирования
// только HID) — после того, как появляется Mass Storage, синхронные
// MSD-передачи выполняются уже ПОСЛЕ того, как клавиатура/мышь используют
// usb_poll() каждый тик, и обе стороны неизбежно дренируют один и тот же
// Event Ring. Поэтому каждое просмотренное, но НЕ совпавшее событие не
// просто отбрасывается — если это HID Transfer Event, оно тут же
// обслуживается через xhci_service_hid_event(), чтобы клавиатура/мышь
// никогда не оставались без перевооружённого TD.
//
// Жёсткий предел итераций внутреннего цикла (в размер кольца) — защита:
// каким бы ни был точный источник, зависание всей системы (usb_poll()
// вызывается из обработчика таймера с запрещёнными прерываниями — если
// здесь зависнуть, останавливаются вообще все тики, включая те, на которых
// держится pit_wait_ms() у любого другого ожидающего кода) недопустимо ни
// при каких обстоятельствах, даже если реальная причина не в этой функции.
// Счётчик активных синхронных ожиданий (want_ptr != 0 — конкретное
// событие: control-передача энумерации, MSD bulk-передача). Пока он > 0,
// usb_poll() (вызывается из обработчика таймера — см. pit.c) не трогает
// Event Ring вообще: сама xhci_wait_for_event_impl() уже дренирует кольцо
// и обслуживает попутные HID-события через xhci_service_hid_event() (см.
// комментарий выше). Без этой защиты периодический usb_poll() мог
// перехватить (want_ptr=0 — "любое" Transfer Event) как раз то событие,
// которое ждёт синхронный вызов — тот таймаутил, хотя контроллер уже
// реально ответил. Найдено и подтверждено этой же сессией: именно эта
// гонка (а не только короткий таймаут и не только баг Cycle-бита Link
// TRB, исправленный выше в xhci_ring_enqueue) — причина того, что mount
// иногда падал уже на самом первом блоке без какой-либо связи с числом
// уже выполненных передач.
static volatile int xhci_sync_wait_depth = 0;

static int xhci_wait_for_event_impl(uint32_t want_type, uint64_t want_ptr,
                                     uint32_t *out_status, uint8_t *out_slot_id,
                                     int timeout_ms)
{
    for (int elapsed = 0; elapsed <= timeout_ms; elapsed++) {
        for (int guard = 0; guard < XHCI_RING_TRB_CAPACITY; guard++) {
            xhci_trb_t *ev = &xhci_event_ring.trbs[xhci_event_ring.dequeue_index];
            if ((ev->control & TRB_CONTROL_CYCLE) != (xhci_event_ring.cycle_state & TRB_CONTROL_CYCLE))
                goto no_new_event; // новых событий нет

            uint32_t type = (uint32_t)TRB_CONTROL_TYPE_GET(ev->control);
            uint32_t status = ev->status;
            uint8_t slot_id = (uint8_t)((ev->control >> TRB_CONTROL_SLOT_SHIFT) & 0xFFu);
            uint64_t param = ev->parameter;

            xhci_event_ring.dequeue_index++;
            if (xhci_event_ring.dequeue_index >= XHCI_RING_TRB_CAPACITY) {
                xhci_event_ring.dequeue_index = 0;
                xhci_event_ring.cycle_state ^= 1;
            }
            xhci_write_erdp();

            if (type == want_type && (want_ptr == 0 || param == want_ptr)) {
                if (out_status) *out_status = status;
                if (out_slot_id) *out_slot_id = slot_id;
                return 0;
            }

            if (type == TRB_TYPE_TRANSFER_EVENT) {
                xhci_service_hid_event(slot_id, status); // не-HID событие тихо игнорируется внутри
            } else {
                DLOG("[XHCI] ignored event type=%u status=%08X\n", type, status);
            }
        }
        printf("[XHCI] WARNING: event ring guard limit hit, giving up this wait\n");
      no_new_event:
        if (want_ptr == 0) return -1; // usb_poll(): "сейчас ничего нет" — не ошибка
        pit_wait_ms(1);
    }
    return -1;
}

static int xhci_wait_for_event(uint32_t want_type, uint64_t want_ptr,
                                uint32_t *out_status, uint8_t *out_slot_id,
                                int timeout_ms)
{
    if (want_ptr != 0) xhci_sync_wait_depth++;
    int rc = xhci_wait_for_event_impl(want_type, want_ptr, out_status, out_slot_id, timeout_ms);
    if (want_ptr != 0) xhci_sync_wait_depth--;
    return rc;
}

static int xhci_wait_command_completion(xhci_trb_t *cmd_trb, uint8_t *out_slot_id, int timeout_ms) {
    uint32_t status;
    uint64_t cmd_phys = xhci_trb_phys(&xhci_cmd_ring, cmd_trb);
    if (xhci_wait_for_event(TRB_TYPE_COMMAND_COMPLETION_EVENT, cmd_phys, &status, out_slot_id, timeout_ms) != 0)
        return -1;
    uint8_t cc = (uint8_t)((status >> TRB_COMPLETION_CODE_SHIFT) & 0xFFu);
    return (cc == TRB_COMPLETION_SUCCESS) ? 0 : -1;
}

/* ======================================================================== */
/* Порты root hub'а                                                         */
/* ======================================================================== */

static uint32_t xhci_portsc_read(uint8_t port) {
    return reg_read32(xhci_op_base, XHCI_OP_PORTSC(port));
}

static void xhci_port_reset(uint8_t port) {
    uint32_t v = xhci_portsc_read(port);
    v &= ~XHCI_PORTSC_RW1C_MASK;
    v |= XHCI_PORTSC_PP; // power on (безопасно писать даже если уже включён)
    v |= XHCI_PORTSC_PR;
    reg_write32(xhci_op_base, XHCI_OP_PORTSC(port), v);

    int timeout = 500;
    while (timeout-- > 0) {
        pit_wait_ms(1);
        if (xhci_portsc_read(port) & XHCI_PORTSC_PRC) break;
    }

    // Сбрасываем PRC (запись 1 сбрасывает), не трогая остальные change-биты.
    v = xhci_portsc_read(port);
    v &= ~XHCI_PORTSC_RW1C_MASK;
    v |= XHCI_PORTSC_PRC;
    reg_write32(xhci_op_base, XHCI_OP_PORTSC(port), v);
}

static int xhci_check_port(uint8_t port, uint8_t *out_speed) {
    uint32_t v = xhci_portsc_read(port);
    if (!(v & XHCI_PORTSC_CCS)) {
        DLOG("[XHCI] Port %u: no device\n", port);
        return 0;
    }

    xhci_port_reset(port);

    v = xhci_portsc_read(port);
    if (!(v & XHCI_PORTSC_PED)) {
        printf("[XHCI] Port %u: failed to enable after reset\n", port);
        return 0;
    }

    *out_speed = (uint8_t)XHCI_PORTSC_SPEED(v);
    DLOG("[XHCI] Port %u: enabled, speed=%u\n", port, *out_speed);
    return 1;
}

/* ======================================================================== */
/* Control-передачи через EP0                                               */
/* ======================================================================== */

// Один общий DMA-буфер, переиспользуемый последовательно — энумерация
// полностью синхронна и обрабатывает один порт целиком, прежде чем
// переходить к следующему (как и раньше в UHCI-драйвере).
static int xhci_control_transfer(xhci_slot_t *slot, const usb_setup_packet_t *setup, void *data_buf) {
    if (!slot->ep0_ring.trbs) return -1;

    uint16_t length = setup->wLength;
    if (length > XHCI_DMA_DATA_MAX) {
        printf("[XHCI] control transfer: length %u too large\n", length);
        return -1;
    }

    int is_in = (setup->bmRequestType & USB_REQ_DIR_IN) != 0;

    uint64_t setup_param;
    memcpy(&setup_param, setup, 8);

    uint32_t trt = (length == 0) ? TRB_TRT_NO_DATA : (is_in ? TRB_TRT_IN : TRB_TRT_OUT);
    xhci_ring_enqueue(&slot->ep0_ring, setup_param, 8,
                       TRB_CONTROL_TYPE_SET(TRB_TYPE_SETUP_STAGE) | TRB_CONTROL_IDT
                       | (trt << TRB_TRT_SHIFT));

    if (length > 0) {
        if (!is_in && data_buf) memcpy(xhci_dma_scratch_virt, data_buf, length);
        xhci_ring_enqueue(&slot->ep0_ring, xhci_dma_scratch_phys, length,
                           TRB_CONTROL_TYPE_SET(TRB_TYPE_DATA_STAGE)
                           | (is_in ? TRB_CONTROL_EP_DIR_IN : 0));
    }

    xhci_trb_t *status_trb = xhci_ring_enqueue(&slot->ep0_ring, 0, 0,
                       TRB_CONTROL_TYPE_SET(TRB_TYPE_STATUS_STAGE) | TRB_CONTROL_IOC
                       | ((length == 0 || !is_in) ? TRB_CONTROL_EP_DIR_IN : 0));
    uint64_t status_trb_phys = xhci_trb_phys(&slot->ep0_ring, status_trb);

    xhci_ring_doorbell(slot->slot_id, XHCI_DCI_CONTROL);

    uint32_t ev_status; uint8_t ev_slot;
    if (xhci_wait_for_event(TRB_TYPE_TRANSFER_EVENT, status_trb_phys, &ev_status, &ev_slot, 500) != 0) {
        printf("[XHCI] control transfer timed out (slot=%u req=0x%02X)\n", slot->slot_id, setup->bRequest);
        return -1;
    }
    uint8_t cc = (uint8_t)((ev_status >> TRB_COMPLETION_CODE_SHIFT) & 0xFFu);
    if (cc != TRB_COMPLETION_SUCCESS) {
        printf("[XHCI] control transfer error cc=%u (slot=%u req=0x%02X)\n", cc, slot->slot_id, setup->bRequest);
        return -1;
    }

    if (is_in && length > 0 && data_buf) memcpy(data_buf, xhci_dma_scratch_virt, length);
    return 0;
}

static int xhci_get_descriptor(xhci_slot_t *slot, uint8_t type, uint8_t index, void *buf, uint16_t len) {
    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQ_DIR_IN | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_DEVICE;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (uint16_t)(((uint16_t)type << 8) | index);
    setup.wIndex = 0;
    setup.wLength = len;
    return xhci_control_transfer(slot, &setup, buf);
}

static int xhci_set_configuration(xhci_slot_t *slot, uint8_t config_value) {
    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQ_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_DEVICE;
    setup.bRequest = USB_REQ_SET_CONFIGURATION;
    setup.wValue = config_value;
    setup.wIndex = 0;
    setup.wLength = 0;
    return xhci_control_transfer(slot, &setup, NULL);
}

static int xhci_set_protocol_boot(xhci_slot_t *slot, uint8_t interface_num) {
    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE;
    setup.bRequest = USB_HID_REQ_SET_PROTOCOL;
    setup.wValue = 0; // 0 = boot protocol
    setup.wIndex = interface_num;
    setup.wLength = 0;
    return xhci_control_transfer(slot, &setup, NULL);
}

// Обновляет EP0 Max Packet Size в Device Context на реальное значение,
// узнанное из полного дескриптора устройства (первый запрос всегда идёт со
// значением "по умолчанию для скорости", которое не всегда совпадает).
// Упрощение: TR Dequeue Pointer/DCS в переданном Endpoint Context
// фактически игнорируются контроллером при Evaluate Context (кроме
// Max Packet Size) — не пересчитываем текущую позицию кольца отдельно.
static int xhci_evaluate_ep0_max_packet(xhci_slot_t *slot, uint16_t real_max_packet) {
    memset(slot->input_ctx_virt, 0, PAGE_SIZE);
    uint32_t *icc = xhci_ctx_at(slot->input_ctx_virt, 0);
    icc[1] = ICC_DW1_ADD_FLAG(1); // A1 — только EP0

    uint32_t *ep0_ctx = xhci_ctx_at(slot->input_ctx_virt, 2); // index 1=Slot, 2=EP0(dci=1)
    xhci_fill_ep_ctx(ep0_ctx, EP_TYPE_CONTROL, real_max_packet, 0, slot->ep0_ring.phys, 1);

    xhci_trb_t *cmd = xhci_ring_enqueue(&xhci_cmd_ring, slot->input_ctx_phys, 0,
        TRB_CONTROL_TYPE_SET(TRB_TYPE_EVALUATE_CONTEXT_CMD)
        | ((uint32_t)slot->slot_id << TRB_CONTROL_SLOT_SHIFT));
    xhci_ring_doorbell(0, 0);

    uint8_t got_slot;
    return xhci_wait_command_completion(cmd, &got_slot, 500);
}

/* ======================================================================== */
/* Configure Endpoint для interrupt IN HID-endpoint'а                       */
/* ======================================================================== */

static int xhci_configure_hid_endpoint(xhci_slot_t *slot, uint8_t ep_addr,
                                        uint16_t max_packet, uint8_t interval_raw)
{
    int dci = (int)(ep_addr & 0x0Fu) * 2 + 1; // IN endpoint
    if (dci < 2 || dci > 31) return -1;

    if (xhci_ring_init(&slot->hid_ep_ring, 0) != 0) return -1;

    memset(slot->input_ctx_virt, 0, PAGE_SIZE);
    uint32_t *icc = xhci_ctx_at(slot->input_ctx_virt, 0);
    icc[1] = ICC_DW1_ADD_FLAG(0) | ICC_DW1_ADD_FLAG((uint32_t)dci); // A0 (slot) + A(dci)

    uint32_t *slot_ctx = xhci_ctx_at(slot->input_ctx_virt, 1);
    xhci_fill_slot_ctx(slot_ctx, slot->speed, slot->port_id, (uint8_t)dci);

    uint32_t *ep_ctx = xhci_ctx_at(slot->input_ctx_virt, 1 + dci);
    uint8_t interval = xhci_compute_interval(slot->speed, interval_raw);
    xhci_fill_ep_ctx(ep_ctx, EP_TYPE_INTERRUPT_IN, max_packet, interval, slot->hid_ep_ring.phys, 1);

    xhci_trb_t *cmd = xhci_ring_enqueue(&xhci_cmd_ring, slot->input_ctx_phys, 0,
        TRB_CONTROL_TYPE_SET(TRB_TYPE_CONFIGURE_ENDPOINT_CMD)
        | ((uint32_t)slot->slot_id << TRB_CONTROL_SLOT_SHIFT));
    xhci_ring_doorbell(0, 0);

    uint8_t got_slot;
    if (xhci_wait_command_completion(cmd, &got_slot, 500) != 0) return -1;

    slot->hid_ep_dci = dci;
    return 0;
}

/* ======================================================================== */
/* USB Mass Storage — Configure Endpoint для пары bulk IN/OUT               */
/* ======================================================================== */

// В отличие от HID (один endpoint на Configure Endpoint Command), тут сразу
// добавляем ОБА bulk-endpoint'а одной командой — Input Control Context
// прекрасно это поддерживает (просто два выставленных Add-бита), и это
// избавляет от необходимости слать команду дважды.
static int xhci_configure_msd_endpoints(xhci_slot_t *slot, uint8_t in_ep, uint16_t in_mp,
                                         uint8_t out_ep, uint16_t out_mp)
{
    int in_dci = (int)(in_ep & 0x0Fu) * 2 + 1;  // IN
    int out_dci = (int)(out_ep & 0x0Fu) * 2;    // OUT
    if (in_dci < 2 || in_dci > 31 || out_dci < 2 || out_dci > 31 || in_dci == out_dci)
        return -1;

    if (xhci_ring_init(&slot->msd_bulk_in_ring, 0) != 0) return -1;
    if (xhci_ring_init(&slot->msd_bulk_out_ring, 0) != 0) return -1;

    int max_dci = (in_dci > out_dci) ? in_dci : out_dci;

    memset(slot->input_ctx_virt, 0, PAGE_SIZE);
    uint32_t *icc = xhci_ctx_at(slot->input_ctx_virt, 0);
    icc[1] = ICC_DW1_ADD_FLAG(0) | ICC_DW1_ADD_FLAG((uint32_t)in_dci) | ICC_DW1_ADD_FLAG((uint32_t)out_dci);

    uint32_t *slot_ctx = xhci_ctx_at(slot->input_ctx_virt, 1);
    xhci_fill_slot_ctx(slot_ctx, slot->speed, slot->port_id, (uint8_t)max_dci);

    uint32_t *in_ctx = xhci_ctx_at(slot->input_ctx_virt, 1 + in_dci);
    xhci_fill_ep_ctx(in_ctx, EP_TYPE_BULK_IN, in_mp ? in_mp : 512, 0, slot->msd_bulk_in_ring.phys, 1);

    uint32_t *out_ctx = xhci_ctx_at(slot->input_ctx_virt, 1 + out_dci);
    xhci_fill_ep_ctx(out_ctx, EP_TYPE_BULK_OUT, out_mp ? out_mp : 512, 0, slot->msd_bulk_out_ring.phys, 1);

    xhci_trb_t *cmd = xhci_ring_enqueue(&xhci_cmd_ring, slot->input_ctx_phys, 0,
        TRB_CONTROL_TYPE_SET(TRB_TYPE_CONFIGURE_ENDPOINT_CMD)
        | ((uint32_t)slot->slot_id << TRB_CONTROL_SLOT_SHIFT));
    xhci_ring_doorbell(0, 0);

    uint8_t got_slot;
    if (xhci_wait_command_completion(cmd, &got_slot, 500) != 0) return -1;

    slot->msd_bulk_in_dci = in_dci;
    slot->msd_bulk_out_dci = out_dci;
    return 0;
}

/* ======================================================================== */
/* USB Mass Storage — Bulk-Only Transport (CBW/данные/CSW) + SCSI           */
/* ======================================================================== */

// Одна bulk-передача — одно Normal TRB (IOC=1), синхронное ожидание её
// Transfer Event. Весь MSD-протокол в этой версии строго
// последовательный (одна операция целиком, прежде чем начинать
// следующую) — как и control-передачи энумерации, этого достаточно и не
// требует параллельного планирования нескольких TRB в очереди.
static int xhci_bulk_transfer(xhci_slot_t *slot, xhci_ring_t *ring, uint8_t dci,
                               uint64_t buf_phys, uint32_t len)
{
    xhci_trb_t *trb = xhci_ring_enqueue(ring, buf_phys, len,
        TRB_CONTROL_TYPE_SET(TRB_TYPE_NORMAL) | TRB_CONTROL_IOC);
    uint64_t trb_phys = xhci_trb_phys(ring, trb);

    DLOG("[XHCI] MSD DEBUG: enqueued TRB phys=0x%lx, ringing doorbell slot=%u dci=%u\n",
           trb_phys, slot->slot_id, dci);
    xhci_ring_doorbell(slot->slot_id, dci);

    uint32_t status; uint8_t got_slot;
    // Таймаут одной bulk-передачи. Раньше здесь стояло 100мс "для быстрой
    // итерации при диагностике" — в 5 раз короче, чем таймаут control-передач
    // (500мс) — и это было основной причиной нестабильности mount/unmount
    // (пере)читывающих сотни/тысячи секторов подряд: под нагрузкой
    // контроллер иногда не укладывался в 100мс на одну bulk-передачу.
    // Приведено к тому же порядку, что и control-передачи.
    if (xhci_wait_for_event(TRB_TYPE_TRANSFER_EVENT, trb_phys, &status, &got_slot, 1000) != 0) {
        printf("[XHCI] MSD bulk transfer timed out (slot=%u dci=%u)\n", slot->slot_id, dci);
        return -1;
    }
    DLOG("[XHCI] MSD DEBUG: got event, status=%08X\n", status);
    uint8_t cc = (uint8_t)((status >> TRB_COMPLETION_CODE_SHIFT) & 0xFFu);
    if (cc != TRB_COMPLETION_SUCCESS) {
        printf("[XHCI] MSD bulk transfer error cc=%u (slot=%u dci=%u)\n", cc, slot->slot_id, dci);
        return -1;
    }
    return 0;
}

// Полный цикл Bulk-Only Transport: CBW (bulk OUT) -> опциональная стадия
// данных (bulk IN либо OUT, смотря по direction_in) -> CSW (bulk IN).
// Переиспользует общий xhci_dma_scratch последовательно для всех трёх
// стадий (к моменту каждого следующего memcpy предыдущая передача уже
// полностью завершена и подтверждена контроллером) — отдельный буфер под
// каждую стадию не нужен, ровно как и в control-передачах энумерации.
static int xhci_msd_command(xhci_slot_t *slot, const uint8_t *cdb, uint8_t cdb_len,
                             void *data_buf, uint32_t data_len, int direction_in)
{
    if (data_len > PAGE_SIZE) {
        printf("[XHCI] MSD: data_len %u too large\n", data_len);
        return -1;
    }

    uint32_t tag = xhci_msd_next_tag++;

    usb_bot_cbw_t *cbw = (usb_bot_cbw_t *)xhci_dma_scratch_virt;
    memset(cbw, 0, sizeof(*cbw));
    cbw->dCBWSignature = USB_BOT_CBW_SIGNATURE;
    cbw->dCBWTag = tag;
    cbw->dCBWDataTransferLength = data_len;
    cbw->bmCBWFlags = direction_in ? USB_BOT_FLAG_DATA_IN : USB_BOT_FLAG_DATA_OUT;
    cbw->bCBWLUN = 0;
    cbw->bCBWCBLength = cdb_len;
    memcpy(cbw->CBWCB, cdb, cdb_len);

    DLOG("[XHCI] MSD DEBUG: sending CBW (dci=%u tag=%u)\n", slot->msd_bulk_out_dci, tag);
    if (xhci_bulk_transfer(slot, &slot->msd_bulk_out_ring, (uint8_t)slot->msd_bulk_out_dci,
                            xhci_dma_scratch_phys, sizeof(*cbw)) != 0)
        return -1;
    DLOG("[XHCI] MSD DEBUG: CBW sent OK\n");

    if (data_len > 0) {
        if (!direction_in && data_buf) memcpy(xhci_dma_scratch_virt, data_buf, data_len);

        xhci_ring_t *data_ring = direction_in ? &slot->msd_bulk_in_ring : &slot->msd_bulk_out_ring;
        uint8_t data_dci = direction_in ? (uint8_t)slot->msd_bulk_in_dci : (uint8_t)slot->msd_bulk_out_dci;
        DLOG("[XHCI] MSD DEBUG: data stage dci=%u len=%u dir_in=%d\n", data_dci, data_len, direction_in);
        if (xhci_bulk_transfer(slot, data_ring, data_dci, xhci_dma_scratch_phys, data_len) != 0)
            return -1;
        DLOG("[XHCI] MSD DEBUG: data stage OK\n");

        if (direction_in && data_buf) memcpy(data_buf, xhci_dma_scratch_virt, data_len);
    }

    DLOG("[XHCI] MSD DEBUG: reading CSW (dci=%u)\n", slot->msd_bulk_in_dci);
    usb_bot_csw_t *csw = (usb_bot_csw_t *)xhci_dma_scratch_virt;
    if (xhci_bulk_transfer(slot, &slot->msd_bulk_in_ring, (uint8_t)slot->msd_bulk_in_dci,
                            xhci_dma_scratch_phys, sizeof(*csw)) != 0)
        return -1;
    DLOG("[XHCI] MSD DEBUG: CSW received OK\n");

    if (csw->dCSWSignature != USB_BOT_CSW_SIGNATURE || csw->dCSWTag != tag) {
        printf("[XHCI] MSD: bad CSW signature/tag\n");
        return -1;
    }
    if (csw->bCSWStatus != USB_BOT_STATUS_OK) {
        printf("[XHCI] MSD: command failed, CSW status=%u\n", csw->bCSWStatus);
        return -1;
    }
    return 0;
}

static int xhci_msd_test_unit_ready(xhci_slot_t *slot) {
    uint8_t cdb[6] = {SCSI_CMD_TEST_UNIT_READY, 0, 0, 0, 0, 0};
    return xhci_msd_command(slot, cdb, sizeof(cdb), NULL, 0, 0);
}

static int xhci_msd_read_capacity(xhci_slot_t *slot, uint32_t *out_max_lba, uint32_t *out_block_size) {
    uint8_t cdb[10] = {SCSI_CMD_READ_CAPACITY10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t resp[8];
    if (xhci_msd_command(slot, cdb, sizeof(cdb), resp, sizeof(resp), 1) != 0) return -1;
    *out_max_lba = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16)
                 | ((uint32_t)resp[2] << 8) | resp[3];
    *out_block_size = ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16)
                     | ((uint32_t)resp[6] << 8) | resp[7];
    return 0;
}

// Вызывается сразу после конфигурации bulk-endpoint'ов. TEST UNIT READY
// нарочно не проверяем на ошибку — многие флешки отвечают NOT READY на
// самую первую команду сразу после Configure Endpoint (обычное дело,
// не признак поломки), а READ CAPACITY чуть погодя уже проходит нормально.
static int xhci_msd_init(xhci_slot_t *slot) {
    xhci_msd_test_unit_ready(slot);

    uint32_t max_lba, block_size;
    if (xhci_msd_read_capacity(slot, &max_lba, &block_size) != 0) {
        printf("[XHCI] Port %u: READ CAPACITY failed\n", slot->port_id);
        return -1;
    }

    if (xhci_msd_device_count_var >= XHCI_MAX_MSD_DEVICES) {
        printf("[XHCI] Port %u: too many Mass Storage devices, ignoring\n", slot->port_id);
        return -1;
    }

    xhci_msd_device_t *d = &xhci_msd_devices[xhci_msd_device_count_var++];
    d->in_use = 1;
    d->slot_id = slot->slot_id;
    d->max_lba = max_lba;
    d->block_size = block_size;

    DLOG("[XHCI] Port %u: Mass Storage ready (slot=%u blocks=%u block_size=%u)\n",
         slot->port_id, slot->slot_id, max_lba + 1, block_size);
    klog("[XHCI] Port %u: Mass Storage ready (slot=%u)", slot->port_id, slot->slot_id);
    return 0;
}

static xhci_slot_t *xhci_msd_slot_for_index(int index) {
    if (index < 0 || index >= xhci_msd_device_count_var || !xhci_msd_devices[index].in_use)
        return NULL;
    return xhci_slot_for_id(xhci_msd_devices[index].slot_id);
}

/* ======================================================================== */
/* Энумерация устройства на порту                                          */
/* ======================================================================== */

static void xhci_enumerate_device(uint8_t port, uint8_t speed) {
    // 1. Enable Slot Command.
    xhci_trb_t *cmd = xhci_ring_enqueue(&xhci_cmd_ring, 0, 0,
        TRB_CONTROL_TYPE_SET(TRB_TYPE_ENABLE_SLOT_CMD));
    xhci_ring_doorbell(0, 0);

    uint8_t slot_id = 0;
    if (xhci_wait_command_completion(cmd, &slot_id, 500) != 0 ||
        slot_id == 0 || slot_id > XHCI_MAX_SLOTS_SUPPORTED)
    {
        printf("[XHCI] Port %u: Enable Slot failed\n", port);
        return;
    }

    xhci_slot_t *slot = &xhci_slots[slot_id - 1];
    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1;
    slot->slot_id = slot_id;
    slot->port_id = port;
    slot->speed = speed;

    // 2. Device Context — контроллер требует, чтобы драйвер выделил её САМ
    // и прописал в DCBAA[slot_id] ДО Address Device Command.
    uint64_t dctx_phys = pmm_alloc_page();
    if (!dctx_phys) { printf("[XHCI] Port %u: out of memory (device context)\n", port); return; }
    slot->device_ctx_phys = dctx_phys;
    slot->device_ctx_virt = phys_to_virt(dctx_phys);
    memset(slot->device_ctx_virt, 0, PAGE_SIZE);
    xhci_dcbaa_virt[slot_id] = dctx_phys;

    // 3. Input Context + EP0 Transfer Ring.
    uint64_t ictx_phys = pmm_alloc_page();
    if (!ictx_phys) { printf("[XHCI] Port %u: out of memory (input context)\n", port); return; }
    slot->input_ctx_phys = ictx_phys;
    slot->input_ctx_virt = phys_to_virt(ictx_phys);
    memset(slot->input_ctx_virt, 0, PAGE_SIZE);

    if (xhci_ring_init(&slot->ep0_ring, 0) != 0) {
        printf("[XHCI] Port %u: out of memory (EP0 ring)\n", port);
        return;
    }

    uint16_t max_packet_guess = xhci_default_ep0_max_packet(speed);
    slot->ep0_max_packet = max_packet_guess;

    uint32_t *icc = xhci_ctx_at(slot->input_ctx_virt, 0);
    icc[1] = ICC_DW1_ADD_FLAG(0) | ICC_DW1_ADD_FLAG(1); // A0 (slot) + A1 (EP0)
    uint32_t *slot_ctx = xhci_ctx_at(slot->input_ctx_virt, 1);
    xhci_fill_slot_ctx(slot_ctx, speed, port, 1);
    uint32_t *ep0_ctx = xhci_ctx_at(slot->input_ctx_virt, 2);
    xhci_fill_ep_ctx(ep0_ctx, EP_TYPE_CONTROL, max_packet_guess, 0, slot->ep0_ring.phys, 1);

    // 4. Address Device Command — контроллер сам назначает USB-адрес
    // (в отличие от UHCI, где адрес назначался явным SET_ADDRESS).
    cmd = xhci_ring_enqueue(&xhci_cmd_ring, ictx_phys, 0,
        TRB_CONTROL_TYPE_SET(TRB_TYPE_ADDRESS_DEVICE_CMD)
        | ((uint32_t)slot_id << TRB_CONTROL_SLOT_SHIFT));
    xhci_ring_doorbell(0, 0);

    uint8_t got_slot;
    if (xhci_wait_command_completion(cmd, &got_slot, 500) != 0) {
        printf("[XHCI] Port %u: Address Device failed\n", port);
        return;
    }

    uint32_t *out_slot_ctx = xhci_ctx_at(slot->device_ctx_virt, 0);
    uint8_t usb_addr = (uint8_t)SLOT_DW3_USB_ADDR(out_slot_ctx[3]);
    DLOG("[XHCI] Port %u: slot %u addressed (usb addr=%u)\n", port, slot_id, usb_addr);

    // 5. Короткий (8 байт) запрос дескриптора устройства — узнать реальный
    // bMaxPacketSize0 и при необходимости поправить его через Evaluate Context.
    usb_device_descriptor_t dev_desc;
    memset(&dev_desc, 0, sizeof(dev_desc));
    if (xhci_get_descriptor(slot, USB_DESC_DEVICE, 0, &dev_desc, 8) != 0) {
        printf("[XHCI] Port %u: failed to get initial device descriptor\n", port);
        return;
    }
    if (dev_desc.bMaxPacketSize0 != 0 && dev_desc.bMaxPacketSize0 != max_packet_guess) {
        if (xhci_evaluate_ep0_max_packet(slot, dev_desc.bMaxPacketSize0) == 0) {
            slot->ep0_max_packet = dev_desc.bMaxPacketSize0;
        }
    }

    // 6. Полный дескриптор устройства.
    if (xhci_get_descriptor(slot, USB_DESC_DEVICE, 0, &dev_desc, sizeof(dev_desc)) != 0) {
        printf("[XHCI] Port %u: failed to get full device descriptor\n", port);
        return;
    }

    DLOG("[XHCI] Port %u: slot=%u vendor=%04X product=%04X class=%02X\n",
         port, slot_id, dev_desc.idVendor, dev_desc.idProduct, dev_desc.bDeviceClass);

    // 7. Заголовок дескриптора конфигурации (9 байт) — узнать полный размер.
    usb_config_descriptor_t cfg_hdr;
    if (xhci_get_descriptor(slot, USB_DESC_CONFIGURATION, 0, &cfg_hdr, sizeof(cfg_hdr)) != 0) {
        printf("[XHCI] Port %u: failed to get config descriptor header\n", port);
        return;
    }
    uint16_t total_len = cfg_hdr.wTotalLength;
    if (total_len < sizeof(cfg_hdr) || total_len > XHCI_DMA_DATA_MAX) {
        printf("[XHCI] Port %u: unreasonable config length %u\n", port, total_len);
        return;
    }

    // 8. Вся конфигурация целиком (config + interface + HID + endpoint дескрипторы подряд).
    uint8_t config_buf[XHCI_DMA_DATA_MAX];
    if (xhci_get_descriptor(slot, USB_DESC_CONFIGURATION, 0, config_buf, total_len) != 0) {
        printf("[XHCI] Port %u: failed to get full config descriptor\n", port);
        return;
    }

    // 9. Ищем первый подходящий интерфейс — либо boot-протокольный HID
    // (клавиатура/мышь, тот же алгоритм, что был в прежнем UHCI-драйвере),
    // либо Mass Storage (Bulk-Only Transport). Для одного устройства в
    // этой версии драйвера они взаимоисключающие.
    int found_hid = 0, found_msd = 0;
    int in_hid_interface = 0, in_msd_interface = 0;
    uint8_t hid_interface_num = 0;
    uint8_t hid_protocol = 0;
    uint8_t hid_ep_addr = 0;
    uint16_t hid_max_packet = 0;
    uint8_t hid_interval_raw = 0;

    uint8_t msd_in_ep = 0, msd_out_ep = 0;
    uint16_t msd_in_max_packet = 0, msd_out_max_packet = 0;

    uint16_t off = 0;
    while ((uint16_t)(off + 2) <= total_len) {
        uint8_t desc_len = config_buf[off];
        uint8_t desc_type = config_buf[off + 1];
        if (desc_len == 0) break;

        if (desc_type == USB_DESC_INTERFACE &&
            (uint16_t)(off + sizeof(usb_interface_descriptor_t)) <= total_len)
        {
            const usb_interface_descriptor_t *iface =
                (const usb_interface_descriptor_t *)&config_buf[off];

            in_hid_interface =
                !found_hid && !found_msd &&
                iface->bInterfaceClass == USB_CLASS_HID &&
                iface->bInterfaceSubClass == USB_HID_SUBCLASS_BOOT &&
                (iface->bInterfaceProtocol == USB_HID_PROTOCOL_KEYBOARD ||
                 iface->bInterfaceProtocol == USB_HID_PROTOCOL_MOUSE);
            if (in_hid_interface) {
                hid_interface_num = iface->bInterfaceNumber;
                hid_protocol = iface->bInterfaceProtocol;
            }

            in_msd_interface =
                !found_hid && !found_msd &&
                iface->bInterfaceClass == USB_CLASS_MSD &&
                iface->bInterfaceSubClass == USB_MSD_SUBCLASS_SCSI &&
                iface->bInterfaceProtocol == USB_MSD_PROTOCOL_BOT;
        } else if (desc_type == USB_DESC_ENDPOINT &&
                   (uint16_t)(off + sizeof(usb_endpoint_descriptor_t)) <= total_len)
        {
            const usb_endpoint_descriptor_t *ep =
                (const usb_endpoint_descriptor_t *)&config_buf[off];

            int is_in_ep = (ep->bEndpointAddress & 0x80) != 0;
            int ep_type = ep->bmAttributes & 0x03;

            if (in_hid_interface && !found_hid && is_in_ep && ep_type == 0x03) {
                hid_ep_addr = ep->bEndpointAddress & 0x0F;
                hid_max_packet = ep->wMaxPacketSize;
                hid_interval_raw = ep->bInterval;
                found_hid = 1;
            } else if (in_msd_interface && !found_msd && ep_type == 0x02) {
                if (is_in_ep) {
                    msd_in_ep = ep->bEndpointAddress & 0x0F;
                    msd_in_max_packet = ep->wMaxPacketSize;
                } else {
                    msd_out_ep = ep->bEndpointAddress & 0x0F;
                    msd_out_max_packet = ep->wMaxPacketSize;
                }
                if (msd_in_ep && msd_out_ep) found_msd = 1;
            }
        }

        off = (uint16_t)(off + desc_len);
    }

    // 10. Переводим устройство в выбранную конфигурацию.
    if (xhci_set_configuration(slot, cfg_hdr.bConfigurationValue) != 0) {
        printf("[XHCI] Port %u: SET_CONFIGURATION failed\n", port);
        return;
    }

    if (found_hid) {
        if (xhci_set_protocol_boot(slot, hid_interface_num) != 0) {
            printf("[XHCI] Port %u: SET_PROTOCOL(boot) failed\n", port);
        }

        // 11. Configure Endpoint Command для найденного interrupt IN endpoint'а.
        if (xhci_configure_hid_endpoint(slot, hid_ep_addr,
                                         hid_max_packet ? hid_max_packet : 8,
                                         hid_interval_raw) != 0)
        {
            printf("[XHCI] Port %u: Configure Endpoint failed\n", port);
            return;
        }

        // 12. Буфер отчёта и первая interrupt IN передача.
        uint64_t report_phys = pmm_alloc_page();
        if (!report_phys) { printf("[XHCI] Port %u: out of memory (HID report buffer)\n", port); return; }
        slot->device_class = XHCI_DEV_CLASS_HID;
        slot->hid_report_buf_phys = report_phys;
        slot->hid_report_buf_virt = phys_to_virt(report_phys);
        memset(slot->hid_report_buf_virt, 0, PAGE_SIZE);
        slot->hid_report_expected_len = 8;
        slot->hid_protocol = hid_protocol;

        xhci_ring_enqueue(&slot->hid_ep_ring, report_phys, slot->hid_report_expected_len,
                           TRB_CONTROL_TYPE_SET(TRB_TYPE_NORMAL) | TRB_CONTROL_IOC);
        xhci_ring_doorbell(slot_id, (uint8_t)slot->hid_ep_dci);

        if (xhci_hid_device_count < XHCI_MAX_HID_DEVICES) {
            xhci_hid_device_t *out = &xhci_hid_devices[xhci_hid_device_count++];
            out->valid = 1;
            out->address = usb_addr;
            out->protocol = hid_protocol;
            out->ep_addr = hid_ep_addr;
            out->max_packet = hid_max_packet;
            out->interval = hid_interval_raw;
            out->low_speed = (speed == XHCI_SPEED_LOW);
        }

        xhci_ready = 1;

        DLOG("[XHCI] Port %u: HID %s ready (slot=%u ep=%u max_packet=%u interval=%ums)\n",
             port, hid_protocol == USB_HID_PROTOCOL_KEYBOARD ? "keyboard" : "mouse",
             slot_id, hid_ep_addr, hid_max_packet, hid_interval_raw);
        klog("[XHCI] Port %u: HID %s ready (slot=%u)", port,
             hid_protocol == USB_HID_PROTOCOL_KEYBOARD ? "keyboard" : "mouse", slot_id);
    } else if (found_msd) {
        if (xhci_configure_msd_endpoints(slot, msd_in_ep, msd_in_max_packet,
                                          msd_out_ep, msd_out_max_packet) != 0)
        {
            printf("[XHCI] Port %u: Configure Endpoint (MSD) failed\n", port);
            return;
        }

        slot->device_class = XHCI_DEV_CLASS_MSD;

        if (xhci_msd_init(slot) != 0) {
            printf("[XHCI] Port %u: Mass Storage init failed\n", port);
            return;
        }
    } else {
        DLOG("[XHCI] Port %u: no supported interface (HID/MSD) found\n", port);
    }
}

/* ======================================================================== */
/* Публичный API                                                            */
/* ======================================================================== */

void xhci_init(void) {
    if (!xhci_find_controller())
        return;

    uint8_t caplen = *(volatile uint8_t *)((uint8_t *)xhci_cap_base + XHCI_CAP_CAPLENGTH);
    uint32_t hcsparams1 = reg_read32(xhci_cap_base, XHCI_CAP_HCSPARAMS1);
    uint32_t hcsparams2 = reg_read32(xhci_cap_base, XHCI_CAP_HCSPARAMS2);
    uint32_t hccparams1 = reg_read32(xhci_cap_base, XHCI_CAP_HCCPARAMS1);
    uint32_t dboff = reg_read32(xhci_cap_base, XHCI_CAP_DBOFF) & ~0x3u;
    uint32_t rtsoff = reg_read32(xhci_cap_base, XHCI_CAP_RTSOFF) & ~0x1Fu;

    xhci_op_base = (uint8_t *)xhci_cap_base + caplen;
    xhci_db_base = (uint8_t *)xhci_cap_base + dboff;
    xhci_rt_base = (uint8_t *)xhci_cap_base + rtsoff;
    xhci_ir0_base = (uint8_t *)xhci_rt_base + XHCI_RT_IR0;

    xhci_context_size = XHCI_HCCPARAMS1_CSZ(hccparams1) ? 64 : 32;
    xhci_max_slots = XHCI_HCSPARAMS1_MAXSLOTS(hcsparams1);
    if (xhci_max_slots > XHCI_MAX_SLOTS_SUPPORTED) xhci_max_slots = XHCI_MAX_SLOTS_SUPPORTED;
    xhci_max_ports = XHCI_HCSPARAMS1_MAXPORTS(hcsparams1);

    if (!(reg_read32(xhci_op_base, XHCI_OP_PAGESIZE) & 1)) {
        printf("[XHCI] WARNING: controller does not report 4KB page support\n");
    }

    DLOG("[XHCI] caplen=%u ctxsize=%u maxslots=%u maxports=%u\n",
         caplen, xhci_context_size, xhci_max_slots, xhci_max_ports);

    if (!xhci_reset()) return;

    if (!xhci_setup_dcbaa()) return;
    if (!xhci_setup_scratchpad(hcsparams2)) return;

    if (xhci_ring_init(&xhci_cmd_ring, 0) != 0) {
        printf("[XHCI] Out of memory for Command Ring\n");
        return;
    }
    reg_write64(xhci_op_base, XHCI_OP_CRCR, xhci_cmd_ring.phys | XHCI_CRCR_RCS);

    if (!xhci_setup_event_ring()) return;

    uint64_t dma_phys = pmm_alloc_page();
    if (!dma_phys) { printf("[XHCI] Out of memory for DMA scratch buffer\n"); return; }
    xhci_dma_scratch_phys = dma_phys;
    xhci_dma_scratch_virt = (uint8_t *)phys_to_virt(dma_phys);
    memset(xhci_dma_scratch_virt, 0, PAGE_SIZE);

    reg_write32(xhci_op_base, XHCI_OP_CONFIG, xhci_max_slots);

    reg_write32(xhci_op_base, XHCI_OP_USBCMD,
                reg_read32(xhci_op_base, XHCI_OP_USBCMD) | XHCI_USBCMD_RS);
    int timeout = 500;
    while (timeout-- > 0 && (reg_read32(xhci_op_base, XHCI_OP_USBSTS) & XHCI_USBSTS_HCH)) {
        pit_wait_ms(1);
    }
    if (reg_read32(xhci_op_base, XHCI_OP_USBSTS) & XHCI_USBSTS_HCH) {
        printf("[XHCI] WARNING: controller did not leave Halted state\n");
        return;
    }

    // Каждый порт энумерируется ПОЛНОСТЬЮ, прежде чем переходить к
    // следующему — тот же принцип, что и раньше в UHCI (на "адресе по
    // умолчанию" в любой момент может отвечать не более одного устройства;
    // у xHCI явного "адреса 0" больше нет, но Address Device Command всё
    // равно должна выполняться для одного слота за раз, пока энумерация
    // синхронна).
    for (uint8_t port = 1; port <= xhci_max_ports; port++) {
        uint8_t speed = 0;
        if (xhci_check_port(port, &speed)) {
            xhci_enumerate_device(port, speed);
        }
    }

    DLOG("[XHCI] Controller ready\n");
    klog("[XHCI] controller ready");
}

const xhci_hid_device_t *xhci_get_hid_device(int index) {
    if (index < 0 || index >= xhci_hid_device_count)
        return NULL;
    if (!xhci_hid_devices[index].valid)
        return NULL;
    return &xhci_hid_devices[index];
}

int xhci_msd_device_count(void) {
    return xhci_msd_device_count_var;
}

int xhci_msd_get_info(int index, uint32_t *out_max_lba, uint32_t *out_block_size) {
    if (index < 0 || index >= xhci_msd_device_count_var || !xhci_msd_devices[index].in_use)
        return -1;
    if (out_max_lba) *out_max_lba = xhci_msd_devices[index].max_lba;
    if (out_block_size) *out_block_size = xhci_msd_devices[index].block_size;
    return 0;
}

int xhci_msd_read_block(int index, uint32_t lba, void *buf, uint32_t block_size) {
    xhci_slot_t *slot = xhci_msd_slot_for_index(index);
    if (!slot || block_size == 0 || block_size > PAGE_SIZE) return -1;

    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_CMD_READ10;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[8] = 1; // transfer length = 1 блок

    return xhci_msd_command(slot, cdb, sizeof(cdb), buf, block_size, 1);
}

int xhci_msd_write_block(int index, uint32_t lba, const void *buf, uint32_t block_size) {
    xhci_slot_t *slot = xhci_msd_slot_for_index(index);
    if (!slot || block_size == 0 || block_size > PAGE_SIZE) return -1;

    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_CMD_WRITE10;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[8] = 1;

    return xhci_msd_command(slot, cdb, sizeof(cdb), (void *)buf, block_size, 0);
}

#define XHCI_POLL_MAX_EVENTS_PER_TICK 8

void usb_poll(void) {
    if (!xhci_ready) return;
    // Синхронное ожидание (control/bulk-передача) уже само дренирует Event
    // Ring и обслуживает попутные HID-события — не лезем сюда, чтобы не
    // перехватить событие, которого ждут они (см. xhci_sync_wait_depth).
    if (xhci_sync_wait_depth > 0) return;

    // want_ptr=0 (любой Transfer Event) означает, что xhci_wait_for_event()
    // возвращает управление на КАЖДОМ событии, не заходя в свою ветку
    // "чужое — обслужить и продолжать драться дальше" (та ветка нужна
    // только когда кто-то ждёт конкретное ДРУГОЕ событие, например MSD
    // control-передача) — поэтому обслуживание тут делает сам usb_poll(),
    // тем же общим хелпером xhci_service_hid_event().
    for (int i = 0; i < XHCI_POLL_MAX_EVENTS_PER_TICK; i++) {
        uint32_t status; uint8_t slot_id;
        if (xhci_wait_for_event(TRB_TYPE_TRANSFER_EVENT, 0, &status, &slot_id, 0) != 0)
            break; // сейчас ничего не готово

        xhci_service_hid_event(slot_id, status); // не-HID (например, MSD) событие тихо игнорируется внутри
    }
}
