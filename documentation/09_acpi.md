# ACPI Subsystem

This document describes the ACPI (Advanced Configuration and Power Interface) implementation in LufiraOS. The ACPI subsystem provides power management and system control capabilities, including system shutdown.

---

## Table of Contents

1. [Overview](#overview)
2. [Key Concepts](#key-concepts)
   - [RSDP (Root System Description Pointer)](#rsdp-root-system-description-pointer)
   - [RSDT and XSDT](#rsdt-and-xsdt)
   - [FADT (Fixed ACPI Description Table)](#fadt-fixed-acpi-description-table)
3. [Data Structures](#data-structures)
   - [RSDP (Revision 1)](#rsdp-revision-1)
   - [RSDP (Revision 2)](#rsdp-revision-2)
   - [SDT Header](#sdt-header)
   - [FADT (Relevant Fields)](#fadt-relevant-fields)
4. [Initialisation Flow](#initialisation-flow)
   - [RSDP Validation](#rsdp-validation)
   - [Table Selection](#table-selection)
   - [FADT Location](#fadt-location)
   - [ACPI Mode Enable](#acpi-mode-enable)
5. [Shutdown Procedure](#shutdown-procedure)
   - [Standard ACPI Shutdown](#standard-acpi-shutdown)
   - [Fallback Methods](#fallback-methods)
6. [Error Handling](#error-handling)
7. [Dependencies](#dependencies)
8. [Future Extensions](#future-extensions)

---

## Overview

The ACPI subsystem provides power management and system control capabilities. It is responsible for:

- **Parsing the RSDP** – locating the Root System Description Pointer provided by the bootloader.
- **Locating the FADT** – finding the Fixed ACPI Description Table for power management registers.
- **Enabling ACPI mode** – switching the system from legacy APM to ACPI power management.
- **System shutdown** – performing a soft-off (S5 state) using ACPI registers.

**Design Philosophy:**
- **Minimal Implementation** – only essential features for shutdown are implemented.
- **Fallback Support** – legacy methods are used if ACPI is not available.
- **Comprehensive Validation** – checksums and signatures are verified before use.

---

## Key Concepts

### RSDP (Root System Description Pointer)

The RSDP is the entry point to the ACPI tables. It is located by the bootloader and passed to the kernel via the `BootInfo` structure. The RSDP contains pointers to the RSDT (or XSDT) and is validated by checking its signature and checksum.

**Signature:** The RSDP has the signature "RSD PTR " (8 characters, including the trailing space).

### RSDT and XSDT

The RSDT (Root System Description Table) and XSDT (Extended System Description Table) contain pointers to other ACPI tables. The difference is that RSDT uses 32-bit addresses, while XSDT uses 64-bit addresses.

**Selection:**
- If the RSDP revision is 2 or higher and the XSDT address is non-zero, the XSDT is used.
- Otherwise, the RSDT is used.

### FADT (Fixed ACPI Description Table)

The FADT (also known as FACP) is the most important ACPI table for power management. It contains:

- **SMI_CMD** – the port used to enable/disable ACPI.
- **ACPI_ENABLE** – the value to write to SMI_CMD to enable ACPI.
- **PM1a_CNT_BLK** – the port used for power management control (S5 shutdown).
- **PM1b_CNT_BLK** – an optional second control port.

---

## Data Structures

### RSDP (Revision 1)

The revision 1 RSDP contains 32-bit addresses.

| Field | Type | Description |
|-------|------|-------------|
| `Signature` | `char[8]` | "RSD PTR " |
| `Checksum` | `uint8_t` | Checksum of the structure |
| `OEMID` | `char[6]` | OEM identifier |
| `Revision` | `uint8_t` | ACPI version (1 for rev1) |
| `RsdtAddress` | `uint32_t` | Physical address of the RSDT |

### RSDP (Revision 2)

The revision 2 RSDP extends revision 1 with 64-bit addresses.

| Field | Type | Description |
|-------|------|-------------|
| `rev1` | `ACPI_RSDP_Rev1` | Revision 1 fields |
| `Length` | `uint32_t` | Length of the structure |
| `XsdtAddress` | `uint64_t` | Physical address of the XSDT |
| `ExtendedChecksum` | `uint8_t` | Extended checksum (of the full structure) |
| `Reserved` | `uint8_t[3]` | Reserved |

### SDT Header

All ACPI tables share a common header.

| Field | Type | Description |
|-------|------|-------------|
| `Signature` | `char[4]` | Table signature (e.g., "FACP", "DSDT") |
| `Length` | `uint32_t` | Length of the table |
| `Revision` | `uint8_t` | Table revision |
| `Checksum` | `uint8_t` | Checksum of the table |
| `OEMID` | `char[6]` | OEM identifier |
| `OEMTableID` | `char[8]` | OEM table identifier |
| `OEMRevision` | `uint32_t` | OEM revision |
| `CreatorID` | `uint32_t` | Creator ID |
| `CreatorRevision` | `uint32_t` | Creator revision |

### FADT (Relevant Fields)

The FADT contains many fields; only those relevant to shutdown are listed.

| Field | Type | Description |
|-------|------|-------------|
| `header` | `ACPI_SDTHeader` | Standard table header |
| `FirmwareCtrl` | `uint32_t` | FIRMWARE_CTRL pointer (FACS) |
| `Dsdt` | `uint32_t` | DSDT pointer |
| `SMI_CMD` | `uint32_t` | SMI command port |
| `ACPI_ENABLE` | `uint8_t` | Value to enable ACPI |
| `ACPI_DISABLE` | `uint8_t` | Value to disable ACPI |
| `PM1a_CNT_BLK` | `uint32_t` | PM1a control register address |
| `PM1b_CNT_BLK` | `uint32_t` | PM1b control register address (optional) |
| `PM1_CNT_LEN` | `uint8_t` | Length of PM1 control registers |

---

## Initialisation Flow

### RSDP Validation

The RSDP is validated by checking:

1. **Signature** – must be "RSD PTR " (8 characters).
2. **Checksum** – the sum of all bytes in the RSDP must be 0 (mod 256).
   - For revision 1: checksum of the first 20 bytes.
   - For revision 2: checksum of the full 36-byte structure.

### Table Selection

After validation, the system chooses between RSDT and XSDT:

1. If the RSDP revision is 2 or higher and the XSDT address is non-zero:
   - Use the XSDT (64-bit addresses).
2. Otherwise:
   - Use the RSDT (32-bit addresses).

The selected table header is validated by checking its checksum.

### FADT Location

The system scans the SDT entries for the FADT table:

1. The table signature is "FACP" (or the older "FADT" signature).
2. Each entry is checked for a matching signature.
3. The table is validated by checking its checksum.
4. The FADT pointer is stored for later use.

### ACPI Mode Enable

If the FADT contains valid SMI_CMD and ACPI_ENABLE fields:

1. Read the PM1a_CNT_BLK register.
2. Check if ACPI is already enabled (SCI_EN bit).
3. If not enabled, write ACPI_ENABLE to SMI_CMD.
4. Poll the PM1a_CNT_BLK for the SCI_EN bit (timeout after 300 attempts).
5. If the bit is set, ACPI is enabled; otherwise, a warning is issued.

**SCI_EN Bit:** The SCI_EN bit (bit 0) in the PM1 control register indicates whether ACPI is enabled.

---

## Shutdown Procedure

### Standard ACPI Shutdown

The shutdown procedure uses the S5 (soft-off) state:

1. **Check readiness** – if ACPI is not ready, use legacy fallback methods.
2. **Prepare the SLP value**:
   - The SLP_TYPa field for S5 is usually 0 (but may vary).
   - The SLP_EN bit (0x2000) must be set to enable shutdown.
3. **Write to PM1a_CNT_BLK**:
   - Read the current register value.
   - Clear the SLP_TYPa and SLP_EN bits.
   - Write the new SLP_TYPa | SLP_EN value.
4. **Write to PM1b_CNT_BLK** (if present):
   - Perform the same operation on the secondary register.
5. **Wait** – the system should power off.

### Fallback Methods

If ACPI is not available or the shutdown does not complete, the system attempts legacy methods:

1. **QEMU shutdown** – write 0x2000 to port 0x604.
2. **VirtualBox shutdown** – write 0x2000 to port 0xB004.
3. **BOCHS shutdown** – write 0xdead to port 0x8900.

These ports are specific to emulators and virtual machines.

---

## Error Handling

| Condition | Action |
|-----------|--------|
| RSDP address is NULL | Print error, return -1. |
| RSDP signature invalid | Print error, return -1. |
| RSDP checksum failed | Print error, return -1. |
| RSDT/XSDT checksum failed | Print error, return -1. |
| FADT not found | Print error, return -1. |
| ACPI enable timeout | Print warning, continue. |
| Shutdown fails | Print message, attempt fallback. |

**Behaviour:** The kernel continues even if ACPI initialisation fails. Warnings are printed, but the system remains usable.

---

## Dependencies

| Dependency | Purpose |
|------------|---------|
| **BootInfo** | Provides the RSDP address from the bootloader. |
| **Port I/O** | `inb`, `outb`, `inw`, `outw` for register access. |
| **Console** | Logging and error messages via `printf`. |
| **Memory Management** | None (ACPI tables are accessed directly). |

---

## Conclusion

The ACPI subsystem provides essential power management capabilities for LufiraOS. While minimal, it successfully enables system shutdown and is designed to be extended with more features in the future. The fallback methods ensure that the system can still be shut down in emulators even if ACPI is not fully functional.

For more details, refer to the source code in `system/acpi/acpi.c` and `system/acpi/acpi.h`.

---

**Document Version:** 1.0  
**Last Updated:** September 2026  
**Project:** LufiraOS