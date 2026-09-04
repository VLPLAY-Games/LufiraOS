#include "drivers/pci/pci.h"
#include "drivers/console/console.h"
#include "lib/string.h"

/* =========================================================
 * PCI device storage
 * ========================================================= */

static pci_device_t pci_devices[PCI_MAX_DEVICES];
static uint32_t pci_device_count = 0;

/* =========================================================
 * Port I/O
 * ========================================================= */

static inline void outb(uint16_t port, uint8_t value)
{
    asm volatile (
        "outb %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline void outw(uint16_t port, uint16_t value)
{
    asm volatile (
        "outw %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline void outl(uint16_t port, uint32_t value)
{
    asm volatile (
        "outl %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    asm volatile (
        "inb %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;

    asm volatile (
        "inw %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;

    asm volatile (
        "inl %1, %0"
        : "=a"(value)
        : "Nd"(port)
    );

    return value;
}

/* =========================================================
 * Build configuration address
 * ========================================================= */

static uint32_t pci_make_address(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset
)
{
    return
        0x80000000U |
        ((uint32_t)bus << 16) |
        ((uint32_t)device << 11) |
        ((uint32_t)function << 8) |
        (offset & 0xFC);
}

/* =========================================================
 * Config read 32
 * ========================================================= */

uint32_t pci_config_read32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset
)
{
    uint32_t address;

    address = pci_make_address(
        bus,
        device,
        function,
        offset
    );

    outl(
        PCI_CONFIG_ADDRESS,
        address
    );

    return inl(
        PCI_CONFIG_DATA
    );
}

/* =========================================================
 * Config read 16
 * ========================================================= */

uint16_t pci_config_read16(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset
)
{
    uint32_t value;
    uint32_t shift;

    value = pci_config_read32(
        bus,
        device,
        function,
        offset
    );

    shift = (offset & 2) * 8;

    return (uint16_t)(
        (value >> shift) & 0xFFFF
    );
}

/* =========================================================
 * Config read 8
 * ========================================================= */

uint8_t pci_config_read8(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset
)
{
    uint32_t value;
    uint32_t shift;

    value = pci_config_read32(
        bus,
        device,
        function,
        offset
    );

    shift = (offset & 3) * 8;

    return (uint8_t)(
        (value >> shift) & 0xFF
    );
}

/* =========================================================
 * Config write 32
 * ========================================================= */

void pci_config_write32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset,
    uint32_t value
)
{
    uint32_t address;

    address = pci_make_address(
        bus,
        device,
        function,
        offset
    );

    outl(
        PCI_CONFIG_ADDRESS,
        address
    );

    outl(
        PCI_CONFIG_DATA,
        value
    );
}

/* =========================================================
 * Config write 16
 * ========================================================= */

void pci_config_write16(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset,
    uint16_t value
)
{
    uint32_t old_value;
    uint32_t shift;
    uint32_t mask;

    old_value = pci_config_read32(
        bus,
        device,
        function,
        offset
    );

    shift = (offset & 2) * 8;
    mask = 0xFFFFU << shift;

    old_value &= ~mask;
    old_value |= ((uint32_t)value << shift);

    pci_config_write32(
        bus,
        device,
        function,
        offset,
        old_value
    );
}

/* =========================================================
 * Config write 8
 * ========================================================= */

void pci_config_write8(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset,
    uint8_t value
)
{
    uint32_t old_value;
    uint32_t shift;
    uint32_t mask;

    old_value = pci_config_read32(
        bus,
        device,
        function,
        offset
    );

    shift = (offset & 3) * 8;
    mask = 0xFFU << shift;

    old_value &= ~mask;
    old_value |= ((uint32_t)value << shift);

    pci_config_write32(
        bus,
        device,
        function,
        offset,
        old_value
    );
}

/* =========================================================
 * Check whether a PCI function exists
 * ========================================================= */

static int pci_function_exists(
    uint8_t bus,
    uint8_t device,
    uint8_t function
)
{
    uint16_t vendor_id;

    vendor_id = pci_config_read16(
        bus,
        device,
        function,
        PCI_VENDOR_ID
    );

    return vendor_id != 0xFFFF;
}

/* =========================================================
 * Read device information
 * ========================================================= */

static void pci_register_device(
    uint8_t bus,
    uint8_t device,
    uint8_t function
)
{
    pci_device_t* dev;

    if (pci_device_count >= PCI_MAX_DEVICES) {
        return;
    }

    dev = &pci_devices[pci_device_count];

    for (uint32_t i = 0; i < sizeof(pci_device_t); i++) {
        ((uint8_t*)dev)[i] = 0;
    }

    dev->bus = bus;
    dev->device = device;
    dev->function = function;

    dev->vendor_id = pci_config_read16(
        bus,
        device,
        function,
        PCI_VENDOR_ID
    );

    dev->device_id = pci_config_read16(
        bus,
        device,
        function,
        PCI_DEVICE_ID
    );

    dev->command = pci_config_read16(
        bus,
        device,
        function,
        PCI_COMMAND
    );

    dev->status = pci_config_read16(
        bus,
        device,
        function,
        PCI_STATUS
    );

    dev->revision_id = pci_config_read8(
        bus,
        device,
        function,
        PCI_REVISION_ID
    );

    dev->prog_if = pci_config_read8(
        bus,
        device,
        function,
        PCI_PROG_IF
    );

    dev->subclass = pci_config_read8(
        bus,
        device,
        function,
        PCI_SUBCLASS
    );

    dev->class_code = pci_config_read8(
        bus,
        device,
        function,
        PCI_CLASS_CODE
    );

    dev->header_type = pci_config_read8(
        bus,
        device,
        function,
        PCI_HEADER_TYPE
    );

    dev->interrupt_line = pci_config_read8(
        bus,
        device,
        function,
        PCI_INTERRUPT_LINE
    );

    dev->interrupt_pin = pci_config_read8(
        bus,
        device,
        function,
        PCI_INTERRUPT_PIN
    );

    pci_device_count++;

    printf(
        "[PCI] %02X:%02X.%u "
        "vendor=%04X "
        "device=%04X "
        "class=%02X:%02X "
        "if=%02X\n",
        bus,
        device,
        function,
        dev->vendor_id,
        dev->device_id,
        dev->class_code,
        dev->subclass,
        dev->prog_if
    );
}

/* =========================================================
 * Scan function
 * ========================================================= */

static void pci_scan_function(
    uint8_t bus,
    uint8_t device,
    uint8_t function
)
{
    if (!pci_function_exists(
            bus,
            device,
            function)) {

        return;
    }

    pci_register_device(
        bus,
        device,
        function
    );
}

/* =========================================================
 * Scan device
 * ========================================================= */

static void pci_scan_device(
    uint8_t bus,
    uint8_t device
)
{
    uint8_t header_type;
    uint8_t function;

    if (!pci_function_exists(
            bus,
            device,
            0)) {

        return;
    }

    header_type = pci_config_read8(
        bus,
        device,
        0,
        PCI_HEADER_TYPE
    );

    pci_scan_function(
        bus,
        device,
        0
    );

    /*
     * Multifunction device.
     */
    if (header_type &
        PCI_HEADER_MULTIFUNCTION) {

        for (function = 1;
             function < 8;
             function++) {

            pci_scan_function(
                bus,
                device,
                function
            );
        }
    }
}

/* =========================================================
 * Scan a bus
 * ========================================================= */

static void pci_scan_bus(
    uint8_t bus
)
{
    uint8_t device;

    for (device = 0;
         device < 32;
         device++) {

        pci_scan_device(
            bus,
            device
        );
    }
}

/* =========================================================
 * Scan complete PCI address space
 * ========================================================= */

void pci_scan(void)
{
    uint16_t bus;

    pci_device_count = 0;

    for (bus = 0;
         bus < 256;
         bus++) {

        pci_scan_bus(
            (uint8_t)bus
        );
    }
}

/* =========================================================
 * PCI init
 * ========================================================= */

void pci_init(void)
{
    pci_device_count = 0;

    printf(
        "[PCI] Initializing PCI subsystem...\n"
    );

    pci_scan();

    printf(
        "[PCI] Found %u device(s)\n",
        pci_device_count
    );
}

/* =========================================================
 * Device count
 * ========================================================= */

uint32_t pci_get_device_count(void)
{
    return pci_device_count;
}

/* =========================================================
 * Get device
 * ========================================================= */

const pci_device_t* pci_get_device(
    uint32_t index
)
{
    if (index >= pci_device_count) {
        return NULL;
    }

    return &pci_devices[index];
}

/* =========================================================
 * Find by vendor/device
 * ========================================================= */

const pci_device_t* pci_find_device(
    uint16_t vendor_id,
    uint16_t device_id
)
{
    uint32_t i;

    for (i = 0;
         i < pci_device_count;
         i++) {

        if (pci_devices[i].vendor_id == vendor_id &&
            pci_devices[i].device_id == device_id) {

            return &pci_devices[i];
        }
    }

    return NULL;
}

/* =========================================================
 * Find by class/subclass
 * ========================================================= */

const pci_device_t* pci_find_class(
    uint8_t class_code,
    uint8_t subclass
)
{
    uint32_t i;

    for (i = 0;
         i < pci_device_count;
         i++) {

        if (pci_devices[i].class_code ==
                class_code &&

            pci_devices[i].subclass ==
                subclass) {

            return &pci_devices[i];
        }
    }

    return NULL;
}

/* =========================================================
 * Find by class/subclass/prog-if
 * ========================================================= */

const pci_device_t* pci_find_class_if(
    uint8_t class_code,
    uint8_t subclass,
    uint8_t prog_if
)
{
    uint32_t i;

    for (i = 0;
         i < pci_device_count;
         i++) {

        if (pci_devices[i].class_code ==
                class_code &&

            pci_devices[i].subclass ==
                subclass &&

            pci_devices[i].prog_if ==
                prog_if) {

            return &pci_devices[i];
        }
    }

    return NULL;
}

/* =========================================================
 * Read BAR size helper
 * ========================================================= */

static uint32_t pci_bar_size32(
    const pci_device_t* dev,
    uint8_t offset
)
{
    uint32_t original;
    uint32_t probe;
    uint32_t mask;

    original = pci_config_read32(
        dev->bus,
        dev->device,
        dev->function,
        offset
    );

    pci_config_write32(
        dev->bus,
        dev->device,
        dev->function,
        offset,
        0xFFFFFFFF
    );

    probe = pci_config_read32(
        dev->bus,
        dev->device,
        dev->function,
        offset
    );

    pci_config_write32(
        dev->bus,
        dev->device,
        dev->function,
        offset,
        original
    );

    if (probe == 0 ||
        probe == 0xFFFFFFFF) {

        return 0;
    }

    /*
     * I/O BAR.
     */
    if (original & PCI_BAR_TYPE_IO) {

        mask = probe & ~0x3U;
    } else {

        mask = probe & ~0xFULL;
    }

    return (~mask) + 1;
}

/* =========================================================
 * Get BAR
 * ========================================================= */

int pci_get_bar(
    const pci_device_t* dev,
    uint8_t bar_index,
    pci_bar_t* bar
)
{
    uint8_t offset;
    uint32_t original;
    uint32_t probe;

    if (!dev || !bar) {
        return -1;
    }

    if (bar_index >= 6) {
        return -2;
    }

    for (uint32_t i = 0; i < sizeof(pci_bar_t); i++) {
        ((uint8_t*)bar)[i] = 0;
    }

    offset =
        PCI_BAR0 +
        (bar_index * 4);

    original = pci_config_read32(
        dev->bus,
        dev->device,
        dev->function,
        offset
    );

    /*
     * BAR is unused.
     */
    if (original == 0) {
        return 0;
    }

    /* -----------------------------------------------------
     * I/O BAR
     * ----------------------------------------------------- */

    if (original & PCI_BAR_TYPE_IO) {

        probe = pci_config_read32(
            dev->bus,
            dev->device,
            dev->function,
            offset
        );

        bar->address =
            (uint64_t)(original & ~0x3U);

        bar->size =
            (uint64_t)pci_bar_size32(
                dev,
                offset
            );

        bar->is_io = 1;
        bar->is_64bit = 0;
        bar->prefetchable = 0;

        return 0;
    }

    /* -----------------------------------------------------
     * Memory BAR
     * ----------------------------------------------------- */

    bar->is_io = 0;

    bar->prefetchable =
        (original & PCI_BAR_PREFETCHABLE)
        ? 1
        : 0;

    /*
     * 64-bit memory BAR.
     */
    if ((original & PCI_BAR_MEMORY_TYPE_MASK) ==
        PCI_BAR_MEMORY_64) {

        uint32_t upper;
        uint32_t probe_low;
        uint32_t probe_high;
        uint64_t original_address;
        uint64_t probe_address;

        if (bar_index >= 5) {
            return -3;
        }

        upper = pci_config_read32(
            dev->bus,
            dev->device,
            dev->function,
            offset + 4
        );

        original_address =
            ((uint64_t)upper << 32) |
            (uint64_t)(original & ~0xFULL);

        /*
         * Probe low.
         */
        pci_config_write32(
            dev->bus,
            dev->device,
            dev->function,
            offset,
            0xFFFFFFFF
        );

        /*
         * Probe high.
         */
        pci_config_write32(
            dev->bus,
            dev->device,
            dev->function,
            offset + 4,
            0xFFFFFFFF
        );

        probe_low = pci_config_read32(
            dev->bus,
            dev->device,
            dev->function,
            offset
        );

        probe_high = pci_config_read32(
            dev->bus,
            dev->device,
            dev->function,
            offset + 4
        );

        /*
         * Restore.
         */
        pci_config_write32(
            dev->bus,
            dev->device,
            dev->function,
            offset,
            original
        );

        pci_config_write32(
            dev->bus,
            dev->device,
            dev->function,
            offset + 4,
            upper
        );

        probe_address =
            ((uint64_t)probe_high << 32) |
            (uint64_t)(probe_low & ~0xFULL);

        if (probe_address == 0 ||
            probe_address == 0xFFFFFFFFFFFFFFFFULL) {

            return -4;
        }

        bar->address =
            original_address;

        bar->size =
            (~probe_address) + 1;

        bar->is_64bit = 1;

        return 0;
    }

    /*
     * 32-bit memory BAR.
     */
    bar->address =
        (uint64_t)(original & ~0xFULL);

    bar->size =
        (uint64_t)pci_bar_size32(
            dev,
            offset
        );

    bar->is_64bit = 0;

    (void)probe;

    return 0;
}

/* =========================================================
 * Get Command register
 * ========================================================= */

uint16_t pci_get_command(
    const pci_device_t* dev
)
{
    if (!dev) {
        return 0;
    }

    return pci_config_read16(
        dev->bus,
        dev->device,
        dev->function,
        PCI_COMMAND
    );
}

/* =========================================================
 * Set Command register
 * ========================================================= */

void pci_set_command(
    const pci_device_t* dev,
    uint16_t command
)
{
    if (!dev) {
        return;
    }

    pci_config_write16(
        dev->bus,
        dev->device,
        dev->function,
        PCI_COMMAND,
        command
    );
}

/* =========================================================
 * Enable I/O decoding
 * ========================================================= */

void pci_enable_io(
    const pci_device_t* dev
)
{
    uint16_t command;

    command = pci_get_command(dev);

    command |= PCI_COMMAND_IO;

    pci_set_command(
        dev,
        command
    );
}

/* =========================================================
 * Enable memory decoding
 * ========================================================= */

void pci_enable_memory(
    const pci_device_t* dev
)
{
    uint16_t command;

    command = pci_get_command(dev);

    command |= PCI_COMMAND_MEMORY;

    pci_set_command(
        dev,
        command
    );
}

/* =========================================================
 * Enable bus mastering
 * ========================================================= */

void pci_enable_bus_master(
    const pci_device_t* dev
)
{
    uint16_t command;

    command = pci_get_command(dev);

    command |= PCI_COMMAND_BUS_MASTER;

    pci_set_command(
        dev,
        command
    );
}

/* =========================================================
 * Disable legacy INTx interrupts
 * ========================================================= */

void pci_disable_interrupts(
    const pci_device_t* dev
)
{
    uint16_t command;

    command = pci_get_command(dev);

    command |= PCI_COMMAND_INT_DISABLE;

    pci_set_command(
        dev,
        command
    );
}