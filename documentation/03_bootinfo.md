# BootInfo Structure

This document describes the `BootInfo` structure, which is the primary data transfer mechanism between the LufiraOS UEFI bootloader and the kernel. It contains all the essential hardware and system information needed for the kernel to initialise properly.

---

## Table of Contents

1. [Overview](#overview)
2. [Structure Definition](#structure-definition)
3. [Field Descriptions](#field-descriptions)
   - [Graphics Fields](#graphics-fields)
   - [Memory Fields](#memory-fields)
   - [Kernel Fields](#kernel-fields)
   - [System Table Fields](#system-table-fields)
   - [Filesystem Fields](#filesystem-fields)
4. [Usage in the Kernel](#usage-in-the-kernel)
5. [Bootloader Filling](#bootloader-filling)
6. [Memory Layout Considerations](#memory-layout-considerations)
7. [Validation and Error Handling](#validation-and-error-handling)
8. [Future Extensions](#future-extensions)

---

## Overview

The `BootInfo` structure is the single most important piece of data passed from the bootloader to the kernel. It encapsulates all the information the kernel needs to initialise its subsystems, including:

- **Graphics**: Framebuffer address, resolution, and pixel format for the console.
- **Memory**: Total RAM and the UEFI memory map for physical memory management.
- **Kernel**: The location and size of the kernel image for memory reservation.
- **System Tables**: ACPI RSDP and SMBIOS addresses for hardware discovery.
- **Filesystem**: A FAT image loaded into memory for the kernel's filesystem.

The structure is defined in `bootinfo.h` and is filled by the bootloader during the UEFI boot process. It is passed to the kernel's entry point as a single pointer.

---

## Structure Definition

The `BootInfo` structure is defined as follows:

```c
typedef struct {
    // Graphics
    uint64_t FrameBufferBase;
    uint64_t FrameBufferSize;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelsPerScanLine;
    uint32_t PixelFormat;

    // Memory
    uint64_t TotalMemory;
    uint64_t MemoryMapSize;
    void* MemoryMap;
    uint32_t MemoryMapDescriptorSize;

    // Kernel
    uint64_t KernelBase;
    uint64_t KernelSize;

    // System Tables
    uint64_t RsdpAddress;
    uint64_t SmbiosAddress;

    // Filesystem
    uint64_t FATImageBase;
    uint64_t FATImageSize;
} BootInfo;
````

svgsvg

**Alignment:** The structure is aligned to 8 bytes to ensure proper access on 64-bit systems.

---

## Field Descriptions

### Graphics Fields

| **FieldTypeDescription** |            |                                                                                                                |
| ------------------------ | ---------- | -------------------------------------------------------------------------------------------------------------- |
| `FrameBufferBase`        | `uint64_t` | Physical address of the linear framebuffer. This is where the kernel writes pixel data for the console.        |
| `FrameBufferSize`        | `uint64_t` | Size of the framebuffer in bytes. Calculated as `HorizontalResolution * VerticalResolution * bytes_per_pixel`. |
| `HorizontalResolution`   | `uint32_t` | Screen width in pixels.                                                                                        |
| `VerticalResolution`     | `uint32_t` | Screen height in pixels.                                                                                       |
| `PixelsPerScanLine`      | `uint32_t` | Number of pixels per scan line. May be larger than `HorizontalResolution` due to padding.                      |
| `PixelFormat`            | `uint32_t` | Indicates the pixel format: `0` for RGB, `1` for BGR. Used for colour conversion.                              |

**Usage:**

- The console driver uses these fields to initialise the framebuffer.
- The PixelFormat is used to convert RGB values to the correct byte order.

---

### Memory Fields

| **FieldTypeDescription**  |            |                                                                                             |
| ------------------------- | ---------- | ------------------------------------------------------------------------------------------- |
| `TotalMemory`             | `uint64_t` | Total usable RAM in bytes. Calculated from the memory map by summing pages of usable types. |
| `MemoryMapSize`           | `uint64_t` | Size of the UEFI memory map in bytes.                                                       |
| `MemoryMap`               | `void*`    | Pointer to the UEFI memory map. This is an array of `EFI_MEMORY_DESCRIPTOR` structures.     |
| `MemoryMapDescriptorSize` | `uint32_t` | Size of each descriptor entry in bytes. Used to iterate through the memory map.             |

**Memory Map Descriptor (UEFI):**

```c
typedef struct {
    uint32_t Type;           // Memory type (EfiConventionalMemory, EfiLoaderCode, etc.)
    uint64_t PhysicalStart;  // Physical address of the memory region
    uint64_t VirtualStart;   // Virtual address (unused by bootloader)
    uint64_t NumberOfPages;  // Number of 4 KiB pages in the region
    uint64_t Attribute;      // Memory attributes (cacheable, write-protected, etc.)
} EFI_MEMORY_DESCRIPTOR;
```

**Memory Types:**

- `EfiConventionalMemory` (7) – Free memory available for the kernel.
- `EfiLoaderCode` (1) – Memory containing bootloader code.
- `EfiLoaderData` (2) – Memory containing bootloader data.
- `EfiBootServicesCode` (3) – Boot services code (freed after ExitBootServices).
- `EfiBootServicesData` (4) – Boot services data (freed after ExitBootServices).
- `EfiRuntimeServicesCode` (5) – Runtime services code (reserved).
- `EfiRuntimeServicesData` (6) – Runtime services data (reserved).
- `EfiReservedMemoryType` (0) – Reserved memory (not usable).
- `EfiUnusableMemory` (8) – Unusable memory (hardware errors).
- `EfiACPIReclaimMemory` (9) – ACPI reclaimable memory.
- `EfiACPIMemoryNVS` (10) – ACPI NVS memory.
- `EfiMemoryMappedIO` (11) – Memory-mapped I/O.
- `EfiMemoryMappedIOPortSpace` (12) – Memory-mapped I/O port space.
- `EfiPalCode` (13) – PAL code (Itanium-specific).

**Usage:**

- The PMM uses the memory map to initialise the physical memory bitmap.
- Only `EfiConventionalMemory` entries are used for general allocation.
- Other types are reserved to prevent the kernel from using them.

---

### Kernel Fields

| **FieldTypeDescription** |            |                                                                                               |
| ------------------------ | ---------- | --------------------------------------------------------------------------------------------- |
| `KernelBase`             | `uint64_t` | Physical address where the kernel was loaded by the bootloader. Typically `0x100000` (1 MiB). |
| `KernelSize`             | `uint64_t` | Size of the kernel binary in bytes. Used for memory reservation.                              |

**Usage:**

- The PMM reserves the kernel memory region to prevent it from being allocated.
- The kernel entry point is at `KernelBase`.

**Note:** The kernel is loaded at a fixed physical address (`0x100000`) to maintain compatibility with the linker script.

---

### System Table Fields

| **FieldTypeDescription** |            |                                                                                  |
| ------------------------ | ---------- | -------------------------------------------------------------------------------- |
| `RsdpAddress`            | `uint64_t` | Physical address of the ACPI Root System Description Pointer. Zero if not found. |
| `SmbiosAddress`          | `uint64_t` | Physical address of the SMBIOS table. Zero if not found.                         |

**ACPI RSDP:**

- The RSDP contains pointers to the RSDT (Root System Description Table) or XSDT (Extended System Description Table).
- It is used by the ACPI driver to locate the FADT and perform system shutdown.

**SMBIOS:**

- The SMBIOS (System Management BIOS) table provides system information (hardware vendor, model, serial numbers, etc.).
- Currently, this field is not used by the kernel but is reserved for future use.

---

### Filesystem Fields

| **FieldTypeDescription** |            |                                                                                            |
| ------------------------ | ---------- | ------------------------------------------------------------------------------------------ |
| `FATImageBase`           | `uint64_t` | Physical address of the FAT filesystem image loaded by the bootloader. Zero if not loaded. |
| `FATImageSize`           | `uint64_t` | Size of the FAT image in bytes. Zero if not loaded.                                        |

**FAT Image:**

- The bootloader can load the entire boot device (or partition) as a FAT image.
- The kernel mounts this image directly from memory without any disk I/O.
- This is used as the root filesystem for the kernel and shell.

**Benefits of Loading FAT as an Image:**

- No disk driver required in the kernel for initial filesystem access.
- Faster access (memory reads instead of disk reads).
- Simpler initialisation (no block device setup).

**Limitations:**

- The image size is limited by available memory.
- The image is read-only unless the kernel has a disk driver to flush changes.

---

## Usage in the Kernel

The `BootInfo` structure is used by several kernel subsystems during initialisation:

| **SubsystemFields UsedPurpose** |                                                                                                                        |                                                         |
| ------------------------------- | ---------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------- |
| Console                         | `FrameBufferBase`, `FrameBufferSize`, `HorizontalResolution`, `VerticalResolution`, `PixelsPerScanLine`, `PixelFormat` | Set up graphical console                                |
| PMM                             | `MemoryMap`, `MemoryMapSize`, `MemoryMapDescriptorSize`, `KernelBase`, `KernelSize`                                    | Physical memory management                              |
| Paging                          | `MemoryMap`                                                                                                            | Determine maximum physical address for identity mapping |
| FAT                             | `FATImageBase`, `FATImageSize`                                                                                         | Mount filesystem                                        |
| ACPI                            | `RsdpAddress`                                                                                                          | Initialise ACPI for shutdown                            |

**Initialisation Order in Kernel:**

1. Console uses framebuffer fields.
2. PMM uses memory map and kernel fields.
3. Paging uses memory map to determine max physical address.
4. Heap uses paging and PMM.
5. FAT uses FAT image fields.
6. ACPI uses RSDP address.
7. VFS uses FAT.
8. Drivers use PCI and PMM.

---

## Bootloader Filling

The bootloader fills the `BootInfo` structure in the following stages:

1. **Graphics**: After initialising the Graphics Output Protocol (GOP), the bootloader stores:
   - `FrameBufferBase` – `gop->Mode->FrameBufferBase`
   - `FrameBufferSize` – `gop->Mode->FrameBufferSize`
   - `HorizontalResolution` – `gop->Mode->Info->HorizontalResolution`
   - `VerticalResolution` – `gop->Mode->Info->VerticalResolution`
   - `PixelsPerScanLine` – `gop->Mode->Info->PixelsPerScanLine`
   - `PixelFormat` – `1` if `PixelBlueGreenRedReserved8BitPerColor`, else `0`
2. **Memory**: The bootloader calls `GetMemoryMap()` twice:
   - First call determines the required buffer size.
   - Second call fills the buffer and stores the map.
   - `TotalMemory` is calculated by summing pages of conventional memory.
3. **Kernel**: The bootloader loads `kernel.bin` at `0x100000`:
   - `KernelBase` – `0x100000` (fixed address)
   - `KernelSize` – Size of `kernel.bin` in bytes (from file info)
4. **System Tables**: The bootloader scans the UEFI Configuration Table:
   - Matches GUIDs for ACPI 1.0, ACPI 2.0, SMBIOS, and SMBIOS 3.0.
   - Stores the table addresses in `RsdpAddress` and `SmbiosAddress`.
5. **FAT Image**: If enabled, the bootloader reads the boot device:
   - `FATImageBase` – Physical address of allocated memory.
   - `FATImageSize` – Size of the loaded image (capped at 256 MiB).

---

## Memory Layout Considerations

The `BootInfo` structure itself is typically located in the bootloader's memory, which is in the lower 4 GiB of physical memory. After the kernel takes over, it must copy or reference the structure carefully.

**Memory Regions:**

- `KernelBase` – Usually `0x100000` (1 MiB).
- `MemoryMap` – Points to UEFI memory map (freed after `ExitBootServices`).
- `FATImageBase` – Points to a memory region allocated by the bootloader.
- `FrameBufferBase` – Points to the framebuffer (physical address).

**Important:** The `MemoryMap` pointer becomes invalid after `ExitBootServices` is called. The kernel must read the memory map before calling `ExitBootServices` (which is done by the bootloader). The bootloader ensures the final memory map is stored before handing control to the kernel.

---

## Validation and Error Handling

The kernel validates some fields of the `BootInfo` structure:

| **FieldValidationError Handling** |                        |                                            |
| --------------------------------- | ---------------------- | ------------------------------------------ |
| `MemoryMap`                       | Non-NULL               | Kernel halts if memory map is missing.     |
| `MemoryMapSize`                   | > 0                    | Kernel halts if size is zero.              |
| `KernelSize`                      | > 0                    | Kernel continues (should always be valid). |
| `FATImageBase`                    | Checked by FAT driver  | Warning printed if image is missing.       |
| `RsdpAddress`                     | Checked by ACPI driver | Warning printed if RSDP is missing.        |

**What Happens on Error:**

- If critical fields are missing (memory map, kernel size), the kernel halts immediately.
- If optional fields are missing (FAT image, RSDP), the kernel prints a warning and continues.

---

## Conclusion

The `BootInfo` structure is the foundation of LufiraOS initialisation. It provides a clean, well-defined interface between the bootloader and the kernel, abstracting away the complexity of UEFI and hardware discovery. By centralising all boot-time information in a single structure, the kernel can be simpler and more portable.

For more details, refer to the source code in `bootinfo.h` and the bootloader implementation in `boot.c`.

---

**Document Version:** 1.0
**Last Updated:** September 2026
**Project:** LufiraOS
