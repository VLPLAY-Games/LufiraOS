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
   - [Generic Address Structure (GAS)](#generic-address-structure-gas)
   - [FADT (Relevant Fields)](#fadt-relevant-fields)
4. [Initialisation Flow](#initialisation-flow)
   - [RSDP Validation](#rsdp-validation)
   - [Table Selection](#table-selection)
   - [FADT Location](#fadt-location)
   - [ACPI Mode Enable](#acpi-mode-enable)
   - [\_S5 Sleep Type Lookup (DSDT)](#_s5-sleep-type-lookup-dsdt)
5. [Shutdown Procedure](#shutdown-procedure)
   - [Standard ACPI Shutdown](#standard-acpi-shutdown)
   - [Fallback Methods](#fallback-methods)
6. [Software Reset (RESET_REG)](#software-reset-reset_reg)
7. [Other Public API](#other-public-api)
8. [Error Handling](#error-handling)
9. [Dependencies](#dependencies)
10. [Future Extensions](#future-extensions)

---

## Overview

The ACPI subsystem provides power management and system control capabilities. It is responsible for:

- **Parsing the RSDP** – locating the Root System Description Pointer provided by the bootloader.
- **Locating the FADT** – finding the Fixed ACPI Description Table for power management registers.
- **Enabling ACPI mode** – switching the system from legacy APM to ACPI power management.
- **System shutdown** – performing a soft-off (S5 state) using ACPI registers, with the real `SLP_TYPa`/`SLP_TYPb` values parsed out of the DSDT's `_S5` package rather than assumed.
- **Software reset** – triggering a reset via the FADT's `RESET_REG` (ACPI 2.0+), as one of several reset methods available to the kernel.
- **Table lookup for other drivers** – exposing a generic by-signature ACPI table lookup (`acpi_find_table_by_signature()`) and the RTC century register index (`acpi_get_rtc_century_register()`), both consumed by other kernel subsystems (`hpet.c`, `rtc.c`).

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

### Generic Address Structure (GAS)

ACPI's standard way of describing a register that may live in system memory, system I/O space, PCI config space, or elsewhere, used by the FADT for `RESET_REG` (see [Software Reset](#software-reset-reset_reg) below) and for the extended (`X_*`) 64-bit variants of the PM1/PM2/GPE block registers (not themselves used by this driver — only `RESET_REG` is).

| Field | Type | Description |
|-------|------|-------------|
| `AddressSpaceId` | `uint8_t` | `0` = System Memory, `1` = System I/O, ... (ACPI spec §5.2.3.2) |
| `RegisterBitWidth` | `uint8_t` | Register width in bits |
| `RegisterBitOffset` | `uint8_t` | Bit offset of the register |
| `AccessSize` | `uint8_t` | Minimum access size |
| `Address` | `uint64_t` | Register address, interpreted per `AddressSpaceId` |

Only `AddressSpaceId` values `0` (System Memory) and `1` (System I/O) are handled; PCI config space and other address spaces the spec permits are rejected cleanly (`acpi_reset()` returns -1) rather than guessed at, since almost no real platform uses them for `RESET_REG`.

### FADT (Relevant Fields)

The FADT contains many fields; only those relevant to shutdown/reset/RTC are listed — the real struct (`ACPI_FADT` in `acpi.h`) is laid out in full down to `ResetValue` (everything after that — the 64-bit `X_*` GAS registers, `SLEEP_CONTROL_REG`/`SLEEP_STATUS_REG`, Hypervisor Vendor ID — is deliberately not modelled, since neither shutdown nor reset uses it).

| Field | Type | Description |
|-------|------|-------------|
| `header` | `ACPI_SDTHeader` | Standard table header |
| `FirmwareCtrl` | `uint32_t` | FIRMWARE_CTRL pointer (FACS) |
| `Dsdt` | `uint32_t` | DSDT pointer — used to locate and parse the `_S5` package (see below) |
| `SMI_CMD` | `uint32_t` | SMI command port |
| `ACPI_ENABLE` | `uint8_t` | Value to enable ACPI |
| `ACPI_DISABLE` | `uint8_t` | Value to disable ACPI |
| `PM1a_CNT_BLK` | `uint32_t` | PM1a control register address |
| `PM1b_CNT_BLK` | `uint32_t` | PM1b control register address (optional) |
| `PM1_CNT_LEN` | `uint8_t` | Length of PM1 control registers |
| `CENTURY` | `uint8_t` | Index of the CMOS/RTC register holding the century — exposed via `acpi_get_rtc_century_register()`, see [Other Public API](#other-public-api) |
| `Flags` | `uint32_t` | Feature flags; bit 10 (`ACPI_FADT_FLAG_RESET_REG_SUPPORTED`) gates whether `ResetReg`/`ResetValue` are trusted |
| `ResetReg` | `ACPI_GAS` | ACPI 2.0+ — register to write for a software reset; valid only if `header.Revision >= 2`, `header.Length` reaches at least this field, and the flag above is set |
| `ResetValue` | `uint8_t` | Value written to `ResetReg` to trigger the reset |

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

### \_S5 Sleep Type Lookup (DSDT)

Immediately after the FADT is located, `acpi_init()` calls `acpi_find_s5_sleep_type()` to find the platform's **real** `SLP_TYPa`/`SLP_TYPb` values for the S5 (soft-off) state, instead of assuming they are always `0`. This matters because `0` only happens to be correct on QEMU/Bochs/VirtualBox — real hardware commonly uses other values (3, 5, 6, 7...), and without this lookup ACPI shutdown would silently do nothing on such a platform and fall straight through to the legacy fallback ports.

This is **not** a general AML interpreter (LufiraOS has none) — it is a targeted byte-pattern scan of the DSDT for the `_S5_` name, same approach used by most hobby OSes without ACPICA:

1. Validate the DSDT's checksum (via `fadt->Dsdt`); skip the lookup if it fails.
2. Scan for the 4-byte ASCII pattern `"_S5_"`.
3. Confirm it's an actual `Name(_S5_, Package{...})` *declaration* — not just a reference to `\_S5` elsewhere (e.g. inside a method) — by checking for a preceding `NameOp` (`0x08`, optionally preceded by a root-prefix `\`) and a following `PackageOp` (`0x12`).
4. Skip the package's `PkgLength`/`NumElements` AML encoding, then read the two sleep-type byte values (each optionally preceded by AML's `BytePrefix`, `0x0A`), shifting each into bits 10-12 of the PM1 control register — the exact bit position `SLP_TYPa`/`SLP_TYPb` need to be OR'd with `ACPI_SLP_EN` at shutdown time.

If no valid `_S5` package is found, `g_slp_typa`/`g_slp_typb` fall back to `0` — the same QEMU/Bochs/VirtualBox-correct default the driver always used before this lookup existed.

---

## Shutdown Procedure

### Standard ACPI Shutdown

The shutdown procedure uses the S5 (soft-off) state:

1. **Check readiness** – if ACPI is not ready, use legacy fallback methods.
2. **Prepare the SLP value**:
   - The SLP_TYPa/SLP_TYPb values used are whatever `acpi_find_s5_sleep_type()` found in the DSDT at `acpi_init()` time (see [\_S5 Sleep Type Lookup](#_s5-sleep-type-lookup-dsdt) above) — `0` only as a fallback when no `_S5` package was found/parseable.
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

## Software Reset (RESET_REG)

`acpi_reset()` performs a software reset through the FADT's `RESET_REG` (ACPI 2.0+), as an alternative to shutdown. It is deliberately conservative — any of the following makes it return -1 immediately, without attempting a write at all:

- ACPI isn't initialised, or no FADT was found.
- `fadt->header.Revision < 2` — `RESET_REG` doesn't exist before ACPI 2.0.
- `fadt->header.Length < 129` — the table is physically too short to contain `ResetReg` at all (guards against a short/old FADT that merely claims revision 2).
- `ACPI_FADT_FLAG_RESET_REG_SUPPORTED` (bit 10 of `Flags`) is not set.
- `ResetReg.Address == 0`.

If all of those pass, it writes `fadt->ResetValue` to the register described by `ResetReg`: `outb()` if `AddressSpaceId` is System I/O, a direct volatile byte store if it's System Memory, and a clean refusal (-1) for any other address space (PCI config space and so on are valid per spec but essentially never used here, and this driver would rather hand back to the caller than guess at the access method).

A successful reset does not return — the function falls through to a short delay and then returns -1 only because the write *didn't* reset the machine. **The caller is expected to fall back to another reset method** (see `command_reboot()`) whenever `acpi_reset()` returns at all.

---

## Other Public API

Two more accessors, both populated once at `acpi_init()` time and consumed by other kernel subsystems rather than used for shutdown/reset directly:

| Function | Description |
|----------|-------------|
| `uint8_t acpi_get_rtc_century_register(void)` | Returns `fadt->CENTURY` — the CMOS register index holding the century byte — or `0` (ACPI's own "not supported" value, and a safe default) if ACPI isn't ready. Used by `rtc.c` to correctly parse CMOS's two-digit year into a full year. |
| `void *acpi_find_table_by_signature(const char *signature)` | Looks up any ACPI table by its 4-byte signature (e.g. `"HPET"`) in the already-parsed RSDT/XSDT from `acpi_init()` — the same table-walking code (`acpi_find_table()`) `acpi_init()` itself uses internally to find the FADT, exposed for other drivers. Returns `NULL` if ACPI isn't initialised or no matching table exists. Used by `hpet.c` to locate the HPET table without re-implementing RSDP/RSDT/XSDT parsing. |

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

The ACPI subsystem provides essential power management capabilities for LufiraOS. It successfully enables system shutdown (with real, DSDT-derived `SLP_TYPa`/`SLP_TYPb` values rather than an assumed `0`) and a software reset path via `RESET_REG`, and acts as a shared ACPI-table lookup service for other drivers (`hpet.c`, and `rtc.c`'s century handling) — while still remaining deliberately minimal and designed to be extended further. The fallback methods ensure that the system can still be shut down in emulators even if ACPI is not fully functional.

For more details, refer to the source code in `system/acpi/acpi.c` and `system/acpi/acpi.h`.

---

**Document Version:** 1.0  
**Last Updated:** September 2026  
**Project:** LufiraOS