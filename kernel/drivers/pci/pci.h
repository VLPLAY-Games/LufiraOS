#pragma once

#include "lib/types.h"

/* =========================================================
 * PCI Configuration Mechanism #1
 * ========================================================= */

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

/* =========================================================
 * Standard PCI configuration space
 * ========================================================= */

#define PCI_VENDOR_ID          0x00
#define PCI_DEVICE_ID          0x02
#define PCI_COMMAND            0x04
#define PCI_STATUS             0x06
#define PCI_REVISION_ID        0x08
#define PCI_PROG_IF            0x09
#define PCI_SUBCLASS           0x0A
#define PCI_CLASS_CODE         0x0B
#define PCI_CACHE_LINE_SIZE    0x0C
#define PCI_LATENCY_TIMER      0x0D
#define PCI_HEADER_TYPE        0x0E
#define PCI_BIST               0x0F

#define PCI_BAR0               0x10
#define PCI_BAR1               0x14
#define PCI_BAR2               0x18
#define PCI_BAR3               0x1C
#define PCI_BAR4               0x20
#define PCI_BAR5               0x24

#define PCI_INTERRUPT_LINE     0x3C
#define PCI_INTERRUPT_PIN      0x3D

/* =========================================================
 * PCI Command register
 * ========================================================= */

#define PCI_COMMAND_IO         (1 << 0)
#define PCI_COMMAND_MEMORY     (1 << 1)
#define PCI_COMMAND_BUS_MASTER (1 << 2)
#define PCI_COMMAND_SPECIAL    (1 << 3)
#define PCI_COMMAND_MEM_WRITE  (1 << 4)
#define PCI_COMMAND_VGA_SNOOP  (1 << 5)
#define PCI_COMMAND_PARITY     (1 << 6)
#define PCI_COMMAND_SERR       (1 << 8)
#define PCI_COMMAND_FAST_B2B   (1 << 9)
#define PCI_COMMAND_INT_DISABLE (1 << 10)

/* =========================================================
 * PCI Status register
 * ========================================================= */

#define PCI_STATUS_CAPABILITIES   (1 << 4)
#define PCI_STATUS_66MHZ          (1 << 5)
#define PCI_STATUS_FAST_B2B       (1 << 7)
#define PCI_STATUS_PARITY_ERROR   (1 << 15)
#define PCI_STATUS_SIG_TARGET_ABORT (1 << 11)
#define PCI_STATUS_REC_TARGET_ABORT (1 << 12)
#define PCI_STATUS_REC_MASTER_ABORT (1 << 13)

/* =========================================================
 * Header types
 * ========================================================= */

#define PCI_HEADER_TYPE_MASK      0x7F
#define PCI_HEADER_TYPE_NORMAL    0x00
#define PCI_HEADER_TYPE_BRIDGE    0x01
#define PCI_HEADER_TYPE_CARDBUS   0x02
#define PCI_HEADER_MULTIFUNCTION  0x80

/* =========================================================
 * BAR
 * ========================================================= */

#define PCI_BAR_TYPE_IO           0x01

#define PCI_BAR_MEMORY_32         0x00
#define PCI_BAR_MEMORY_64         0x04

#define PCI_BAR_MEMORY_TYPE_MASK  0x06
#define PCI_BAR_PREFETCHABLE      0x08

/* =========================================================
 * Limits
 * ========================================================= */

#define PCI_MAX_DEVICES           256

/* =========================================================
 * PCI device
 * ========================================================= */

typedef struct
{
    uint8_t bus;
    uint8_t device;
    uint8_t function;

    uint16_t vendor_id;
    uint16_t device_id;

    uint16_t command;
    uint16_t status;

    uint8_t revision_id;
    uint8_t prog_if;
    uint8_t subclass;
    uint8_t class_code;

    uint8_t header_type;

    uint8_t interrupt_line;
    uint8_t interrupt_pin;
} pci_device_t;

/* =========================================================
 * BAR information
 * ========================================================= */

typedef struct
{
    uint64_t address;
    uint64_t size;

    uint8_t is_io;
    uint8_t is_64bit;
    uint8_t prefetchable;
} pci_bar_t;

/* =========================================================
 * Raw configuration access
 * ========================================================= */

uint32_t pci_config_read32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset
);

uint16_t pci_config_read16(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset
);

uint8_t pci_config_read8(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset
);

void pci_config_write32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset,
    uint32_t value
);

void pci_config_write16(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset,
    uint16_t value
);

void pci_config_write8(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset,
    uint8_t value
);

/* =========================================================
 * Initialization / enumeration
 * ========================================================= */

void pci_init(void);
void pci_scan(void);

/* =========================================================
 * Device list
 * ========================================================= */

uint32_t pci_get_device_count(void);

const pci_device_t* pci_get_device(
    uint32_t index
);

/* =========================================================
 * Device search
 * ========================================================= */

const pci_device_t* pci_find_device(
    uint16_t vendor_id,
    uint16_t device_id
);

const pci_device_t* pci_find_class(
    uint8_t class_code,
    uint8_t subclass
);

const pci_device_t* pci_find_class_if(
    uint8_t class_code,
    uint8_t subclass,
    uint8_t prog_if
);

/* =========================================================
 * BAR
 * ========================================================= */

int pci_get_bar(
    const pci_device_t* device,
    uint8_t bar_index,
    pci_bar_t* bar
);

/* =========================================================
 * Command register helpers
 * ========================================================= */

uint16_t pci_get_command(
    const pci_device_t* device
);

void pci_set_command(
    const pci_device_t* device,
    uint16_t command
);

void pci_enable_io(
    const pci_device_t* device
);

void pci_enable_memory(
    const pci_device_t* device
);

void pci_enable_bus_master(
    const pci_device_t* device
);

void pci_disable_interrupts(
    const pci_device_t* device
);