#include "uhci.h"

#include "drivers/pci/pci.h"
#include "system/mm/pmm.h"
#include "system/timer/pit.h"
#include "drivers/console/console.h"

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
/* Состояние драйвера                                                       */
/* ======================================================================== */

static uint16_t uhci_io_base = 0;
static const pci_device_t *uhci_dev = NULL;
static uint64_t uhci_frame_list_phys = 0;

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
    uint32_t *frame_list = (uint32_t *)(uintptr_t)phys;
    for (int i = 0; i < 1024; i++) {
        frame_list[i] = 0x00000001; // Terminate bit — слот пуст
    }

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

    uhci_port_reset(port_reg);

    val = uhci_in16(port_reg);
    if (val & UHCI_PORTSC_PE) {
        printf("[UHCI] Port %d: enabled\n", index);
    } else {
        printf("[UHCI] Port %d: failed to enable after reset\n", index);
    }
}

/* ======================================================================== */
/* Публичный API                                                            */
/* ======================================================================== */

void uhci_init(void) {
    if (!uhci_find_controller())
        return;

    uhci_reset();

    if (!uhci_setup_frame_list())
        return;

    uhci_check_port(0, uhci_io_base + UHCI_PORTSC1);
    uhci_check_port(1, uhci_io_base + UHCI_PORTSC2);

    printf("[UHCI] Controller ready (no device transfers yet)\n");
}

void usb_poll(void) {
    if (!uhci_ready)
        return;

    // Периодическое расписание (клавиатура/мышь) появится в следующих
    // фазах — пока опрашивать нечего.
}
