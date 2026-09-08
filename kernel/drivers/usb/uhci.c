#include "uhci.h"
#include "usb.h"
#include "usb_hid.h"

#include "drivers/pci/pci.h"
#include "system/mm/pmm.h"
#include "system/timer/pit.h"
#include "drivers/console/console.h"
#include "lib/string.h"

/* ======================================================================== */
/* Port I/O                                                                  */
/* ======================================================================== */

static inline void uhci_out8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void uhci_out16(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void uhci_out32(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t uhci_in16(uint16_t port) {
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

/* ======================================================================== */
/* Регистры UHCI (смещения от io_base)                                      */
/* ======================================================================== */

#define UHCI_USBCMD    0x00  // 16-bit
#define UHCI_USBSTS    0x02  // 16-bit
#define UHCI_USBINTR   0x04  // 16-bit
#define UHCI_FRNUM     0x06  // 16-bit
#define UHCI_FRBASEADD 0x08  // 32-bit, должен быть выровнен по 4KB
#define UHCI_SOFMOD    0x0C  // 8-bit
#define UHCI_PORTSC1   0x10  // 16-bit
#define UHCI_PORTSC2   0x12  // 16-bit

#define UHCI_USBCMD_RS      (1 << 0)  // Run/Stop
#define UHCI_USBCMD_HCRESET (1 << 1)  // Host Controller Reset (самоочищается)
#define UHCI_USBCMD_GRESET  (1 << 2)  // Global Reset
#define UHCI_USBCMD_CF      (1 << 6)  // Configure Flag (чисто информационный)

#define UHCI_PORTSC_CCS  (1 << 0)  // Current Connect Status (RO)
#define UHCI_PORTSC_CSC  (1 << 1)  // Connect Status Change (R/WC)
#define UHCI_PORTSC_PE   (1 << 2)  // Port Enable (R/W)
#define UHCI_PORTSC_PEC  (1 << 3)  // Port Enable Change (R/WC)
#define UHCI_PORTSC_LSDA (1 << 8)  // Low Speed Device Attached (RO)
#define UHCI_PORTSC_PR   (1 << 9)  // Port Reset (R/W)

// Биты, которые НЕЛЬЗЯ бездумно переписывать при read-modify-write:
// CSC/PEC - "запись 1 сбрасывает", случайно записав туда 1 просто потому
// что мы читали текущее значение, можно стереть ещё не обработанное
// событие подключения/отключения.
#define UHCI_PORTSC_RWC_MASK (UHCI_PORTSC_CSC | UHCI_PORTSC_PEC)

// PCI config offset регистра USB Legacy Support (Intel-совместимые
// UHCI-контроллеры, включая эмулируемый QEMU PIIX3).
#define UHCI_PCI_USBLEGSUP 0xC0

/* ======================================================================== */
/* Link Pointer (используется и в Frame List, и в TD.link, и в QH)          */
/* ======================================================================== */

#define UHCI_LINK_TERMINATE (1 << 0) // T: 1 = конец, дальше ничего нет
#define UHCI_LINK_QH        (1 << 1) // Q: 1 = указывает на QH, 0 = на TD
#define UHCI_LINK_VF        (1 << 2) // Vf: 1 = depth-first (сразу перейти к тому, на что указываем)

/* ======================================================================== */
/* Transfer Descriptor (TD) — 16 байт, аппаратно значимых, плюс запас       */
/* до 32 байт для удобного выравнивания пула.                              */
/* ======================================================================== */

typedef struct __attribute__((packed, aligned(16))) {
    volatile uint32_t link;
    volatile uint32_t status;
    volatile uint32_t token;
    volatile uint32_t buffer;
    uint32_t sw_reserved[4]; // не используется контроллером, только для выравнивания
} uhci_td_t;

// TD.status (DWORD 1) — точные позиции битов из UHCI 1.1 Design Guide
// (ошибочно были сдвинуты на 3 бита ниже при первой реализации: Active
// писался в бит 20 вместо 23, из-за чего для full-speed устройств
// реальный Active-бит контроллера вообще никогда не выставлялся, и
// control-передачи гарантированно "висели" до таймаута).
#define UHCI_TD_STATUS_CERR_SHIFT 27
#define UHCI_TD_STATUS_LS       (1 << 26) // Low Speed Device
#define UHCI_TD_STATUS_IOC      (1 << 24) // Interrupt on Complete
#define UHCI_TD_STATUS_ACTIVE   (1 << 23)
#define UHCI_TD_STATUS_STALLED  (1 << 22)
#define UHCI_TD_STATUS_DBUFERR  (1 << 21)
#define UHCI_TD_STATUS_BABBLE   (1 << 20)
#define UHCI_TD_STATUS_NAK      (1 << 19)
#define UHCI_TD_STATUS_CRCTO    (1 << 18)
#define UHCI_TD_STATUS_BITSTUFF (1 << 17)
#define UHCI_TD_STATUS_ERROR_MASK \
    (UHCI_TD_STATUS_STALLED | UHCI_TD_STATUS_DBUFERR | UHCI_TD_STATUS_BABBLE | \
     UHCI_TD_STATUS_CRCTO | UHCI_TD_STATUS_BITSTUFF)
     // NAK намеренно не входит в "жёсткую" ошибку — на NAK'е контроллер
     // сам бесконечно повторяет попытку (не тратя C_ERR), это ловится
     // общим таймаутом ожидания, а не этим флагом.

// TD.token (DWORD 2)
#define UHCI_PID_SETUP 0x2D
#define UHCI_PID_IN    0x69
#define UHCI_PID_OUT   0xE1

/* ======================================================================== */
/* Queue Head (QH) — 8 байт аппаратно значимых, плюс запас до 16.           */
/* ======================================================================== */

typedef struct __attribute__((packed, aligned(16))) {
    volatile uint32_t head_link;    // горизонтальная связь (следующий QH в расписании)
    volatile uint32_t element_link; // первый TD в очереди этого QH
    uint32_t sw_reserved[2];
} uhci_qh_t;

/* ======================================================================== */
/* Пулы TD/QH и DMA-буфер под control-передачи (энумерация)                 */
/* ======================================================================== */

#define UHCI_TD_POOL_CAP 48   // с запасом: даже конфигурация ~256 байт при
                              // 8-байтном max packet — это 32 TD данных + setup + status
#define UHCI_QH_POOL_CAP 4
#define UHCI_DMA_DATA_MAX 256 // с запасом под device+config+interface+HID+endpoint дескрипторы

static uhci_td_t *uhci_td_pool = NULL;
static int uhci_td_pool_next = 0;

static uhci_qh_t *uhci_qh_pool = NULL;

// [0..16) — копия setup-пакета, [16..16+UHCI_DMA_DATA_MAX) — данные.
static uint8_t *uhci_dma_scratch = NULL;

typedef struct {
    int connected;
    int enabled;
    int low_speed;
} uhci_port_state_t;

static uhci_port_state_t uhci_ports[2];
static uhci_hid_device_t uhci_hid_devices[2];

/* ======================================================================== */
/* Периодическая (interrupt) передача клавиатуры                            */
/* ======================================================================== */

// Отдельная страница под TD/буферы периодических передач — их нельзя
// держать в uhci_td_pool, потому что control_transfer() на каждый свой
// вызов переиспользует его с самого начала (uhci_td_pool_next = 0),
// затерев чужой постоянно висящий в расписании TD.
#define UHCI_PERIODIC_KBD_TD_OFFSET  0    // uhci_td_t, 32 байта
#define UHCI_PERIODIC_MOUSE_TD_OFFSET 32  // зарезервировано под фазу E
#define UHCI_PERIODIC_KBD_BUF_OFFSET  128 // 8-байтный boot-отчёт клавиатуры
#define UHCI_PERIODIC_MOUSE_BUF_OFFSET 144 // зарезервировано под фазу E

static uint8_t *uhci_periodic_pool = NULL;

static uhci_qh_t *uhci_kbd_qh = NULL;
static uhci_td_t *uhci_kbd_td = NULL;
static uint8_t   *uhci_kbd_buf = NULL;
static uint8_t uhci_kbd_addr = 0;
static uint8_t uhci_kbd_ep = 0;
static uint8_t uhci_kbd_max_packet = 8;
static int uhci_kbd_low_speed = 0;
static int uhci_kbd_toggle = 0;

/* ======================================================================== */
/* Состояние драйвера                                                       */
/* ======================================================================== */

static uint16_t uhci_io_base = 0;
static const pci_device_t *uhci_dev = NULL;
static uint64_t uhci_frame_list_phys = 0;
static uint32_t *uhci_frame_list = NULL;

// true, когда более поздняя фаза установит периодическое расписание
// (interrupt-QH клавиатуры/мыши) и usb_poll() начнёт реально что-то делать.
static int uhci_ready = 0;

/* ======================================================================== */
/* PCI-обнаружение                                                          */
/* ======================================================================== */

static int uhci_find_controller(void) {
    // class 0x0C = Serial Bus Controller, subclass 0x03 = USB, prog-if 0x00 = UHCI
    const pci_device_t *dev = pci_find_class_if(0x0C, 0x03, 0x00);

    if (!dev) {
        printf("[UHCI] Controller not found\n");
        return 0;
    }

    printf("[UHCI] Controller found at %u:%u.%u (vendor=%04X device=%04X)\n",
           dev->bus, dev->device, dev->function,
           dev->vendor_id, dev->device_id);

    pci_bar_t bar4;
    if (pci_get_bar(dev, 4, &bar4) != 0) {
        printf("[UHCI] Failed to read BAR4\n");
        return 0;
    }

    if (!bar4.is_io) {
        printf("[UHCI] Expected an I/O BAR at BAR4\n");
        return 0;
    }

    if (bar4.address > 0xFFFF) {
        printf("[UHCI] Invalid I/O BAR address\n");
        return 0;
    }

    uhci_dev = dev;
    uhci_io_base = (uint16_t)bar4.address;

    printf("[UHCI] I/O base = %04X\n", uhci_io_base);

    // Отключаем перехват контроллера через SMI (BIOS legacy PS/2-эмуляция) —
    // иначе прошивка может продолжать "владеть" контроллером параллельно с нами.
    pci_config_write16(dev->bus, dev->device, dev->function,
                        UHCI_PCI_USBLEGSUP, 0x8F00);

    pci_enable_io(dev);
    pci_enable_bus_master(dev);

    // Опрашиваем сами (как AC'97) — PCI IRQ этого контроллера не используем.
    pci_disable_interrupts(dev);

    return 1;
}

/* ======================================================================== */
/* Сброс контроллера                                                        */
/* ======================================================================== */

static void uhci_reset(void) {
    // Останавливаем, если контроллер уже был запущен (например, BIOS'ом).
    uhci_out16(uhci_io_base + UHCI_USBCMD, 0);
    pit_wait_ms(1);

    // Global Reset.
    uhci_out16(uhci_io_base + UHCI_USBCMD, UHCI_USBCMD_GRESET);
    pit_wait_ms(10);
    uhci_out16(uhci_io_base + UHCI_USBCMD, 0);
    pit_wait_ms(1);

    // Host Controller Reset — бит самоочищается, когда сброс завершён.
    uhci_out16(uhci_io_base + UHCI_USBCMD, UHCI_USBCMD_HCRESET);

    int timeout = 100; // 100 x 1мс = 100мс с запасом
    while ((uhci_in16(uhci_io_base + UHCI_USBCMD) & UHCI_USBCMD_HCRESET) &&
           timeout-- > 0)
    {
        pit_wait_ms(1);
    }

    if (uhci_in16(uhci_io_base + UHCI_USBCMD) & UHCI_USBCMD_HCRESET) {
        printf("[UHCI] WARNING: Host Controller Reset did not complete\n");
    }
}

/* ======================================================================== */
/* Frame List (расписание на 1024 кадра, пока пустое)                       */
/* ======================================================================== */

// Переписывает ВСЕ 1024 слота Frame List так, чтобы каждый указывал на
// начало цепочки QH. Пока клавиатурный interrupt-QH не настроен (uhci_kbd_qh
// == NULL), первой (и единственной) в цепочке остаётся control QH — ровно
// как было в фазах A/B/C. Как только uhci_start_keyboard_interrupt()
// подключит interrupt-QH перед control QH (через его head_link), эта же
// функция вызывается повторно, и расписание становится
// interrupt(kbd) -> control.
static void uhci_relink_schedule(void) {
    uhci_qh_t *first = uhci_kbd_qh ? uhci_kbd_qh : &uhci_qh_pool[0];
    uint32_t link = (((uint32_t)(uintptr_t)first) & ~0xFu) | UHCI_LINK_QH;
    for (int i = 0; i < 1024; i++) {
        uhci_frame_list[i] = link;
    }
}

static int uhci_setup_frame_list(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
        printf("[UHCI] Out of memory for Frame List\n");
        return 0;
    }

    if (phys > 0xFFFFFFFFULL) {
        // UHCI умеет адресовать только 32-битные физические адреса.
        printf("[UHCI] Frame List page above 4GB, cannot use\n");
        pmm_free_page(phys);
        return 0;
    }

    uhci_frame_list_phys = phys;

    // Физическая память в этом ядре identity-mapped, так что можно писать
    // напрямую по физическому адресу.
    uhci_frame_list = (uint32_t *)(uintptr_t)phys;

    // Постоянно линкуем control QH в КАЖДЫЙ слот расписания. Если
    // подвешивать его только в frame_list[0] (и снимать после каждой
    // передачи), контроллер сам крутит FRNUM по кругу на 1024 кадра —
    // слот 0 обслуживается примерно раз в секунду, и 100мс-таймаут
    // control-передачи почти гарантированно её не застаёт (что и
    // вызывало "control transfer timed out" на каждой энумерации).
    // Держа QH во всех слотах, на каждую передачу меняем только
    // qh->element_link — контроллер подхватывает её уже на следующем
    // кадре (<=1мс).
    uhci_qh_pool[0].head_link = UHCI_LINK_TERMINATE;
    uhci_qh_pool[0].element_link = UHCI_LINK_TERMINATE;
    uhci_relink_schedule();

    uhci_out16(uhci_io_base + UHCI_FRNUM, 0);
    uhci_out32(uhci_io_base + UHCI_FRBASEADD, (uint32_t)uhci_frame_list_phys);
    uhci_out8(uhci_io_base + UHCI_SOFMOD, 0x40);

    // Сбрасываем все прежние биты состояния (запись 1 сбрасывает) и не
    // используем HC-прерывания — опрашиваем сами.
    uhci_out16(uhci_io_base + UHCI_USBSTS, 0xFFFF);
    uhci_out16(uhci_io_base + UHCI_USBINTR, 0);

    // Запускаем контроллер обрабатывать (пока пустое) расписание.
    uhci_out16(uhci_io_base + UHCI_USBCMD, UHCI_USBCMD_RS | UHCI_USBCMD_CF);

    return 1;
}

/* ======================================================================== */
/* Порты root hub'а                                                         */
/* ======================================================================== */

static void uhci_port_reset(uint16_t port_reg) {
    uint16_t val = uhci_in16(port_reg);
    val &= ~UHCI_PORTSC_RWC_MASK;
    val |= UHCI_PORTSC_PR;
    uhci_out16(port_reg, val);

    pit_wait_ms(50); // USB-спецификация: минимум 50мс на assertion Port Reset

    val = uhci_in16(port_reg);
    val &= ~UHCI_PORTSC_RWC_MASK;
    val &= ~UHCI_PORTSC_PR;
    uhci_out16(port_reg, val);

    pit_wait_ms(10); // recovery time

    val = uhci_in16(port_reg);
    val &= ~UHCI_PORTSC_RWC_MASK;
    val |= UHCI_PORTSC_PE;
    uhci_out16(port_reg, val);

    pit_wait_ms(10);
}

static void uhci_check_port(int index, uint16_t port_reg) {
    uint16_t val = uhci_in16(port_reg);

    if (!(val & UHCI_PORTSC_CCS)) {
        printf("[UHCI] Port %d: no device\n", index);
        return;
    }

    int low_speed = (val & UHCI_PORTSC_LSDA) ? 1 : 0;
    printf("[UHCI] Port %d: device connected (%s-speed)\n",
           index, low_speed ? "low" : "full");

    uhci_ports[index].connected = 1;
    uhci_ports[index].low_speed = low_speed;

    uhci_port_reset(port_reg);

    val = uhci_in16(port_reg);
    if (val & UHCI_PORTSC_PE) {
        printf("[UHCI] Port %d: enabled\n", index);
        uhci_ports[index].enabled = 1;
    } else {
        printf("[UHCI] Port %d: failed to enable after reset\n", index);
    }
}

/* ======================================================================== */
/* Пулы TD/QH/DMA-буфера под control-передачи                               */
/* ======================================================================== */

static int uhci_setup_transfer_pools(void) {
    uint64_t td_phys = pmm_alloc_page();
    uint64_t qh_phys = pmm_alloc_page();
    uint64_t dma_phys = pmm_alloc_page();

    if (!td_phys || !qh_phys || !dma_phys ||
        td_phys > 0xFFFFFFFFULL || qh_phys > 0xFFFFFFFFULL || dma_phys > 0xFFFFFFFFULL)
    {
        printf("[UHCI] Failed to allocate transfer pools\n");
        if (td_phys) pmm_free_page(td_phys);
        if (qh_phys) pmm_free_page(qh_phys);
        if (dma_phys) pmm_free_page(dma_phys);
        return 0;
    }

    uhci_td_pool = (uhci_td_t *)(uintptr_t)td_phys;
    uhci_qh_pool = (uhci_qh_t *)(uintptr_t)qh_phys;
    uhci_dma_scratch = (uint8_t *)(uintptr_t)dma_phys;

    memset(uhci_td_pool, 0, 4096);   // pmm_alloc_page() всегда даёт ровно одну 4KB-страницу
    memset(uhci_qh_pool, 0, 4096);
    memset(uhci_dma_scratch, 0, 4096);

    return 1;
}

static uhci_td_t *uhci_alloc_td(void) {
    if (uhci_td_pool_next >= UHCI_TD_POOL_CAP) {
        printf("[UHCI] TD pool exhausted\n");
        return NULL;
    }
    uhci_td_t *td = &uhci_td_pool[uhci_td_pool_next++];
    td->link = UHCI_LINK_TERMINATE;
    td->status = 0;
    td->token = 0;
    td->buffer = 0;
    return td;
}

static uint32_t uhci_td_make_status(int low_speed) {
    uint32_t s = 0;
    s |= (3u << UHCI_TD_STATUS_CERR_SHIFT); // C_ERR = 3 (максимум аппаратных повторов)
    if (low_speed) s |= UHCI_TD_STATUS_LS;
    s |= UHCI_TD_STATUS_ACTIVE;
    return s;
}

static uint32_t uhci_td_make_token(uint8_t pid, uint8_t addr, uint8_t ep,
                                   int toggle, uint16_t len)
{
    uint32_t maxlen = (len == 0) ? 0x7FFu : (uint32_t)(len - 1);
    uint32_t token = 0;
    token |= (maxlen & 0x7FFu) << 21;
    token |= (uint32_t)(toggle & 1) << 19;
    token |= ((uint32_t)ep & 0xFu) << 15;
    token |= ((uint32_t)addr & 0x7Fu) << 8;
    token |= pid;
    return token;
}

// Связывает td с next в цепочке ОДНОЙ очереди (depth-first: контроллер
// сразу переходит к следующему TD, а не возвращается в Frame List).
static void uhci_td_link_to(uhci_td_t *td, uhci_td_t *next) {
    uint32_t next_phys = (uint32_t)(uintptr_t)next;
    td->link = (next_phys & ~0xFu) | UHCI_LINK_VF;
}

/* ======================================================================== */
/* Control-передача: SETUP TD + 0..N DATA TD + STATUS TD                    */
/* ======================================================================== */

// data_out: при IN-запросе сюда копируются полученные данные (после
// завершения передачи); при OUT-запросе отсюда данные копируются В
// передачу. Может быть NULL, если setup->wLength == 0. Возвращает 0 при
// успехе, -1 при ошибке/таймауте.
static int uhci_control_transfer(uint8_t addr, const usb_setup_packet_t *setup,
                                 void *data_out, uint8_t max_packet, int low_speed)
{
    if (!uhci_td_pool || !uhci_qh_pool || !uhci_dma_scratch || !uhci_frame_list)
        return -1;

    if (max_packet == 0) max_packet = 8;

    uint16_t length = setup->wLength;
    if (length > UHCI_DMA_DATA_MAX) {
        printf("[UHCI] control transfer: length %u too large\n", length);
        return -1;
    }

    int is_in = (setup->bmRequestType & USB_REQ_DIR_IN) != 0;

    // Один control-transfer единовременно — пул просто переиспользуется.
    uhci_td_pool_next = 0;

    memcpy(uhci_dma_scratch, setup, sizeof(usb_setup_packet_t));
    uint32_t setup_phys = (uint32_t)(uintptr_t)uhci_dma_scratch;

    uint8_t *dma_data = uhci_dma_scratch + 16;
    uint32_t dma_data_phys = (uint32_t)(uintptr_t)dma_data;

    if (!is_in && length > 0 && data_out) {
        memcpy(dma_data, data_out, length);
    }

    uhci_td_t *setup_td = uhci_alloc_td();
    if (!setup_td) return -1;
    setup_td->token = uhci_td_make_token(UHCI_PID_SETUP, addr, 0, 0, 8);
    setup_td->buffer = setup_phys;
    setup_td->status = uhci_td_make_status(low_speed);

    uhci_td_t *prev = setup_td;
    int toggle = 1; // первый пакет данных после SETUP — всегда DATA1
    uint8_t data_pid = is_in ? UHCI_PID_IN : UHCI_PID_OUT;

    uint16_t remaining = length;
    uint32_t offset = 0;

    while (remaining > 0) {
        uint16_t chunk = remaining > max_packet ? max_packet : remaining;

        uhci_td_t *td = uhci_alloc_td();
        if (!td) return -1;
        td->token = uhci_td_make_token(data_pid, addr, 0, toggle, chunk);
        td->buffer = dma_data_phys + offset;
        td->status = uhci_td_make_status(low_speed);
        uhci_td_link_to(prev, td);

        prev = td;
        toggle ^= 1;
        remaining -= chunk;
        offset += chunk;
    }

    // Status-стадия: если данных не было — всегда IN; иначе — противоположно
    // направлению стадии данных. Toggle стадии статуса всегда DATA1
    // (независимо от того, на чём остановилось чередование в стадии данных).
    uint8_t status_pid = (length == 0) ? UHCI_PID_IN : (is_in ? UHCI_PID_OUT : UHCI_PID_IN);

    uhci_td_t *status_td = uhci_alloc_td();
    if (!status_td) return -1;
    status_td->token = uhci_td_make_token(status_pid, addr, 0, 1, 0);
    status_td->buffer = 0;
    status_td->status = uhci_td_make_status(low_speed);
    uhci_td_link_to(prev, status_td);
    status_td->link = UHCI_LINK_TERMINATE;

    // Control QH уже постоянно висит в каждом слоте Frame List (см.
    // uhci_setup_frame_list()) — подключаем цепочку TD к нему, ничего не
    // трогая в самом Frame List, чтобы контроллер увидел её уже на
    // следующем кадре, а не раз в 1024 кадра.
    uhci_qh_t *qh = &uhci_qh_pool[0];
    qh->element_link = ((uint32_t)(uintptr_t)setup_td) & ~0xFu;

    uint32_t qh_elem_before = qh->element_link;

    int done = 0;
    for (int timeout = 0; timeout < 100; timeout++) { // до ~100мс
        if (!(status_td->status & UHCI_TD_STATUS_ACTIVE)) { done = 1; break; }
        pit_wait_ms(1);
    }

    uint32_t qh_elem_after = qh->element_link;
    qh->element_link = UHCI_LINK_TERMINATE; // снимаем цепочку TD с QH

    if (!done) {
        printf("[UHCI] control transfer timed out (addr=%u req=0x%02X)\n", addr, setup->bRequest);
        printf("[UHCI]   USBSTS=%04X USBCMD=%04X FRNUM=%04X qh.elem before=%08X after=%08X (setup_td phys=%08X)\n",
               uhci_in16(uhci_io_base + UHCI_USBSTS),
               uhci_in16(uhci_io_base + UHCI_USBCMD),
               uhci_in16(uhci_io_base + UHCI_FRNUM),
               qh_elem_before, qh_elem_after,
               (uint32_t)(uintptr_t)setup_td);
        for (int i = 0; i < uhci_td_pool_next; i++) {
            uint32_t st = uhci_td_pool[i].status;
            printf("[UHCI]   TD %d: %s%s%s%s%s%s%s CERR=%u actlen=%u token=%08X\n",
                   i,
                   (st & UHCI_TD_STATUS_ACTIVE)   ? "ACTIVE "   : "",
                   (st & UHCI_TD_STATUS_STALLED)  ? "STALL "    : "",
                   (st & UHCI_TD_STATUS_DBUFERR)  ? "DBUFERR "  : "",
                   (st & UHCI_TD_STATUS_BABBLE)   ? "BABBLE "   : "",
                   (st & UHCI_TD_STATUS_NAK)      ? "NAK "      : "",
                   (st & UHCI_TD_STATUS_CRCTO)    ? "CRC/TO "   : "",
                   (st & UHCI_TD_STATUS_BITSTUFF) ? "BITSTUFF " : "",
                   (st >> UHCI_TD_STATUS_CERR_SHIFT) & 0x3,
                   st & 0x7FFu,
                   uhci_td_pool[i].token);
        }
        return -1;
    }

    for (int i = 0; i < uhci_td_pool_next; i++) {
        uint32_t st = uhci_td_pool[i].status;
        if (st & UHCI_TD_STATUS_ACTIVE) {
            printf("[UHCI] control transfer: TD %d stuck active (addr=%u req=0x%02X)\n",
                   i, addr, setup->bRequest);
            return -1;
        }
        if (st & UHCI_TD_STATUS_ERROR_MASK) {
            printf("[UHCI] control transfer error 0x%08X on TD %d (addr=%u req=0x%02X)\n",
                   st, i, addr, setup->bRequest);
            return -1;
        }
    }

    if (is_in && length > 0 && data_out) {
        memcpy(data_out, dma_data, length);
    }

    return 0;
}

/* ======================================================================== */
/* Стандартные device requests поверх control_transfer                      */
/* ======================================================================== */

static int uhci_get_descriptor(uint8_t addr, uint8_t type, uint8_t index,
                               void *buf, uint16_t len, uint8_t max_packet, int low_speed)
{
    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQ_DIR_IN | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_DEVICE;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (uint16_t)(((uint16_t)type << 8) | index);
    setup.wIndex = 0;
    setup.wLength = len;
    return uhci_control_transfer(addr, &setup, buf, max_packet, low_speed);
}

static int uhci_set_address(uint8_t new_addr, uint8_t max_packet, int low_speed) {
    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQ_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_DEVICE;
    setup.bRequest = USB_REQ_SET_ADDRESS;
    setup.wValue = new_addr;
    setup.wIndex = 0;
    setup.wLength = 0;
    // SET_ADDRESS выполняется, пока устройство ЕЩЁ на адресе 0 — новый
    // адрес передаётся в wValue, а не в адресате самой передачи.
    return uhci_control_transfer(0, &setup, NULL, max_packet, low_speed);
}

static int uhci_set_configuration(uint8_t addr, uint8_t config_value,
                                  uint8_t max_packet, int low_speed)
{
    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQ_DIR_OUT | USB_REQ_TYPE_STANDARD | USB_REQ_RECIP_DEVICE;
    setup.bRequest = USB_REQ_SET_CONFIGURATION;
    setup.wValue = config_value;
    setup.wIndex = 0;
    setup.wLength = 0;
    return uhci_control_transfer(addr, &setup, NULL, max_packet, low_speed);
}

static int uhci_set_protocol_boot(uint8_t addr, uint8_t interface_num,
                                  uint8_t max_packet, int low_speed)
{
    usb_setup_packet_t setup;
    setup.bmRequestType = USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_INTERFACE;
    setup.bRequest = USB_HID_REQ_SET_PROTOCOL;
    setup.wValue = 0; // 0 = boot protocol
    setup.wIndex = interface_num;
    setup.wLength = 0;
    return uhci_control_transfer(addr, &setup, NULL, max_packet, low_speed);
}

/* ======================================================================== */
/* Энумерация устройства на порту                                          */
/* ======================================================================== */

static void uhci_enumerate_device(int port_index) {
    int low_speed = uhci_ports[port_index].low_speed;
    uint8_t max_packet = 8; // безопасное значение для самого первого запроса

    usb_device_descriptor_t dev_desc;
    memset(&dev_desc, 0, sizeof(dev_desc));

    // 1. Короткий запрос дескриптора устройства (8 байт) на адресе 0 —
    // этого достаточно, чтобы узнать bMaxPacketSize0 для EP0.
    if (uhci_get_descriptor(0, USB_DESC_DEVICE, 0, &dev_desc, 8, max_packet, low_speed) != 0) {
        printf("[UHCI] Port %d: failed to get initial device descriptor\n", port_index);
        return;
    }

    if (dev_desc.bMaxPacketSize0 != 0) {
        max_packet = dev_desc.bMaxPacketSize0;
    }

    // 2. Назначаем адрес (у нас всего 2 порта, поэтому 1/2 гарантированно уникальны).
    uint8_t new_addr = (uint8_t)(port_index + 1);
    if (uhci_set_address(new_addr, max_packet, low_speed) != 0) {
        printf("[UHCI] Port %d: SET_ADDRESS failed\n", port_index);
        return;
    }
    pit_wait_ms(10); // рекомендованная спецификацией пауза после SET_ADDRESS

    // 3. Полный дескриптор устройства — уже на новом адресе.
    if (uhci_get_descriptor(new_addr, USB_DESC_DEVICE, 0, &dev_desc,
                            sizeof(dev_desc), max_packet, low_speed) != 0)
    {
        printf("[UHCI] Port %d: failed to get full device descriptor\n", port_index);
        return;
    }

    printf("[UHCI] Port %d: addr=%u vendor=%04X product=%04X class=%02X\n",
           port_index, new_addr, dev_desc.idVendor, dev_desc.idProduct,
           dev_desc.bDeviceClass);

    // 4. Заголовок дескриптора конфигурации (9 байт) — чтобы узнать полный размер.
    usb_config_descriptor_t cfg_hdr;
    if (uhci_get_descriptor(new_addr, USB_DESC_CONFIGURATION, 0, &cfg_hdr,
                            sizeof(cfg_hdr), max_packet, low_speed) != 0)
    {
        printf("[UHCI] Port %d: failed to get config descriptor header\n", port_index);
        return;
    }

    uint16_t total_len = cfg_hdr.wTotalLength;
    if (total_len < sizeof(cfg_hdr) || total_len > UHCI_DMA_DATA_MAX) {
        printf("[UHCI] Port %d: unreasonable config length %u\n", port_index, total_len);
        return;
    }

    // 5. Вся конфигурация целиком (config + interface + HID + endpoint дескрипторы подряд).
    uint8_t config_buf[UHCI_DMA_DATA_MAX];
    if (uhci_get_descriptor(new_addr, USB_DESC_CONFIGURATION, 0, config_buf,
                            total_len, max_packet, low_speed) != 0)
    {
        printf("[UHCI] Port %d: failed to get full config descriptor\n", port_index);
        return;
    }

    // 6. Ищем первый boot-протокольный HID-интерфейс (клавиатура/мышь) и
    // его interrupt IN endpoint.
    int found_hid = 0;
    int in_target_interface = 0;
    uint8_t hid_interface_num = 0;
    uhci_hid_device_t hid;
    memset(&hid, 0, sizeof(hid));

    uint16_t off = 0;
    while ((uint16_t)(off + 2) <= total_len) {
        uint8_t desc_len = config_buf[off];
        uint8_t desc_type = config_buf[off + 1];

        if (desc_len == 0) break; // защита от зависания на битом дескрипторе

        if (desc_type == USB_DESC_INTERFACE &&
            (uint16_t)(off + sizeof(usb_interface_descriptor_t)) <= total_len)
        {
            const usb_interface_descriptor_t *iface =
                (const usb_interface_descriptor_t *)&config_buf[off];

            in_target_interface =
                !found_hid &&
                iface->bInterfaceClass == USB_CLASS_HID &&
                iface->bInterfaceSubClass == USB_HID_SUBCLASS_BOOT &&
                (iface->bInterfaceProtocol == USB_HID_PROTOCOL_KEYBOARD ||
                 iface->bInterfaceProtocol == USB_HID_PROTOCOL_MOUSE);

            if (in_target_interface) {
                hid_interface_num = iface->bInterfaceNumber;
                hid.protocol = iface->bInterfaceProtocol;
            }
        } else if (desc_type == USB_DESC_ENDPOINT && in_target_interface && !found_hid &&
                   (uint16_t)(off + sizeof(usb_endpoint_descriptor_t)) <= total_len)
        {
            const usb_endpoint_descriptor_t *ep =
                (const usb_endpoint_descriptor_t *)&config_buf[off];

            int is_in_ep = (ep->bEndpointAddress & 0x80) != 0;
            int is_interrupt_ep = (ep->bmAttributes & 0x03) == 0x03;

            if (is_in_ep && is_interrupt_ep) {
                hid.valid = 1;
                hid.address = new_addr;
                hid.ep_addr = ep->bEndpointAddress & 0x0F;
                hid.max_packet = ep->wMaxPacketSize;
                hid.interval = ep->bInterval;
                hid.low_speed = low_speed;
                found_hid = 1;
            }
        }

        off = (uint16_t)(off + desc_len);
    }

    // 7. Переводим устройство в выбранную конфигурацию.
    if (uhci_set_configuration(new_addr, cfg_hdr.bConfigurationValue,
                               max_packet, low_speed) != 0)
    {
        printf("[UHCI] Port %d: SET_CONFIGURATION failed\n", port_index);
        return;
    }
    pit_wait_ms(5);

    if (found_hid) {
        // Явно фиксируем boot-протокол (мы и так запрашивали только
        // boot-совместимые интерфейсы, но это belt-and-suspenders).
        if (uhci_set_protocol_boot(new_addr, hid_interface_num, max_packet, low_speed) != 0) {
            printf("[UHCI] Port %d: SET_PROTOCOL(boot) failed\n", port_index);
        }

        uhci_hid_devices[port_index] = hid;

        printf("[UHCI] Port %d: HID %s ready (addr=%u ep=%u max_packet=%u interval=%ums)\n",
               port_index,
               hid.protocol == USB_HID_PROTOCOL_KEYBOARD ? "keyboard" : "mouse",
               hid.address, hid.ep_addr, hid.max_packet, hid.interval);
    } else {
        printf("[UHCI] Port %d: no boot-protocol HID interface found\n", port_index);
    }
}

/* ======================================================================== */
/* Периодическая (interrupt) передача клавиатуры                            */
/* ======================================================================== */

static int uhci_setup_periodic_pool(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys || phys > 0xFFFFFFFFULL) {
        printf("[UHCI] Failed to allocate periodic transfer pool\n");
        if (phys) pmm_free_page(phys);
        return 0;
    }

    uhci_periodic_pool = (uint8_t *)(uintptr_t)phys;
    memset(uhci_periodic_pool, 0, 4096);
    return 1;
}

// Настраивает постоянный interrupt-QH для найденной клавиатуры и подвешивает
// на него один самопереустанавливающийся TD, читающий 8-байтный
// boot-отчёт с её endpoint'а. QH подключается ПЕРЕД control QH (через
// head_link) и остаётся в расписании навсегда — usb_poll() лишь
// перевооружает TD, когда предыдущая передача завершается.
static int uhci_start_keyboard_interrupt(const uhci_hid_device_t *dev) {
    if (!uhci_periodic_pool && !uhci_setup_periodic_pool())
        return -1;

    uhci_kbd_qh  = &uhci_qh_pool[1];
    uhci_kbd_td  = (uhci_td_t *)(uhci_periodic_pool + UHCI_PERIODIC_KBD_TD_OFFSET);
    uhci_kbd_buf = uhci_periodic_pool + UHCI_PERIODIC_KBD_BUF_OFFSET;

    uhci_kbd_addr = dev->address;
    uhci_kbd_ep = dev->ep_addr;
    uhci_kbd_max_packet = (dev->max_packet == 0 || dev->max_packet > 8) ? 8 : (uint8_t)dev->max_packet;
    uhci_kbd_low_speed = dev->low_speed;
    // SET_CONFIGURATION в uhci_enumerate_device() сбрасывает data toggle
    // этого endpoint'а на аппарате в DATA0 — начинаем с того же значения.
    uhci_kbd_toggle = 0;

    uhci_kbd_qh->head_link = (((uint32_t)(uintptr_t)&uhci_qh_pool[0]) & ~0xFu) | UHCI_LINK_QH;
    uhci_kbd_qh->element_link = UHCI_LINK_TERMINATE;

    uhci_kbd_td->link = UHCI_LINK_TERMINATE;
    uhci_kbd_td->token = uhci_td_make_token(UHCI_PID_IN, uhci_kbd_addr, uhci_kbd_ep,
                                            uhci_kbd_toggle, uhci_kbd_max_packet);
    uhci_kbd_td->buffer = (uint32_t)(uintptr_t)uhci_kbd_buf;
    uhci_kbd_td->status = uhci_td_make_status(uhci_kbd_low_speed);

    uhci_kbd_qh->element_link = ((uint32_t)(uintptr_t)uhci_kbd_td) & ~0xFu;

    // Расписание было interrupt(kbd) отсутствует -> control; теперь
    // становится interrupt(kbd) -> control.
    uhci_relink_schedule();

    return 0;
}

/* ======================================================================== */
/* Публичный API                                                            */
/* ======================================================================== */

void uhci_init(void) {
    if (!uhci_find_controller())
        return;

    uhci_reset();

    // Пулы (в частности uhci_qh_pool) должны существовать ДО построения
    // Frame List — она линкует control QH в каждый свой слот сразу же.
    if (!uhci_setup_transfer_pools())
        return;

    if (!uhci_setup_frame_list())
        return;

    // Порт 0 энумерируется (и, что важно, переводится с адреса 0 на
    // собственный) ПОЛНОСТЬЮ, прежде чем трогать порт 1 — на адресе 0 в
    // любой момент может "откликаться" не более одного нового устройства.
    uhci_check_port(0, uhci_io_base + UHCI_PORTSC1);
    if (uhci_ports[0].enabled) {
        uhci_enumerate_device(0);
    }

    uhci_check_port(1, uhci_io_base + UHCI_PORTSC2);
    if (uhci_ports[1].enabled) {
        uhci_enumerate_device(1);
    }

    // Пока поддерживаем одну активную клавиатуру одновременно — первая
    // найденная выигрывает (моделирование двух параллельных interrupt-QH
    // клавиатур не входит в объём этой фазы).
    for (int i = 0; i < 2; i++) {
        if (uhci_hid_devices[i].valid &&
            uhci_hid_devices[i].protocol == USB_HID_PROTOCOL_KEYBOARD)
        {
            if (uhci_start_keyboard_interrupt(&uhci_hid_devices[i]) == 0) {
                uhci_ready = 1;
                printf("[UHCI] Port %d: keyboard interrupt transfer armed\n", i);
            } else {
                printf("[UHCI] Port %d: failed to arm keyboard interrupt transfer\n", i);
            }
            break;
        }
    }

    printf("[UHCI] Controller ready\n");
}

const uhci_hid_device_t *uhci_get_hid_device(int port_index) {
    if (port_index < 0 || port_index > 1)
        return NULL;
    if (!uhci_hid_devices[port_index].valid)
        return NULL;
    return &uhci_hid_devices[port_index];
}

void usb_poll(void) {
    if (!uhci_ready)
        return;

    if (!uhci_kbd_td)
        return;

    if (uhci_kbd_td->status & UHCI_TD_STATUS_ACTIVE)
        return;

    uint32_t st = uhci_kbd_td->status;

    if (!(st & UHCI_TD_STATUS_ERROR_MASK)) {
        usb_hid_keyboard_report(uhci_kbd_buf);
        uhci_kbd_toggle ^= 1;
    }

    /*
     * После completion UHCI продвигает QH.element_link.
     * Для единственного TD он обычно становится TERMINATE.
     *
     * Поэтому для повторной interrupt-передачи нужно не только
     * перевооружить сам TD, но и снова прикрепить его к QH.
     */
    uhci_kbd_qh->element_link = UHCI_LINK_TERMINATE;

    uhci_kbd_td->token = uhci_td_make_token(
        UHCI_PID_IN,
        uhci_kbd_addr,
        uhci_kbd_ep,
        uhci_kbd_toggle,
        uhci_kbd_max_packet
    );

    uhci_kbd_td->status = uhci_td_make_status(uhci_kbd_low_speed);

    /*
     * Убедимся, что новый TD полностью записан до того,
     * как снова публикуем его через QH.
     */
    __asm__ volatile ("" ::: "memory");

    uhci_kbd_qh->element_link =
        ((uint32_t)(uintptr_t)uhci_kbd_td) & ~0xFu;
}
