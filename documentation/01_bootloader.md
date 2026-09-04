# LufiraOS Bootloader Documentation

Welcome to the LufiraOS project! This document provides a comprehensive overview of the bootloader subsystem, its architecture, components, and usage. The bootloader is a UEFI application written in C that prepares the system environment, loads the kernel, and transfers control to it. It supports multiple boot modes, graphical splash screens, and detailed diagnostics.

---

## Table of Contents

1. [Overview](#overview)
2. [Bootloader Architecture](#bootloader-architecture)
   - [Boot Flow](#boot-flow)
   - [Boot Modes](#boot-modes)
   - [Boot Information Structure (`BootInfo`)](#boot-information-structure-bootinfo)
   - [UEFI Services Usage](#uefi-services-usage)
3. [Bootloader Components](#bootloader-components)
   - [Main Entry (`boot.c`)](#main-entry-bootc)
   - [Boot Mode Implementations](#boot-mode-implementations)
     - [Normal Mode (`quick_boot.c`)](#normal-mode-quick_bootc)
     - [Debug Mode (`debug_boot.c`)](#debug-mode-debug_bootc)
     - [Safe Mode (`safe_boot.c`)](#safe-mode-safe_bootc)
   - [Loaders](#loaders)
     - [Kernel Loader (`kernel_loader.c`)](#kernel-loader-kernel_loaderc)
     - [FAT Image Loader (`fat_loader.c`)](#fat-image-loader-fat_loaderc)
   - [System Services](#system-services)
     - [Memory Management (`memory.c`)](#memory-management-memoryc)
     - [Graphics Initialization (`graphics.c`)](#graphics-initialization-graphicsc)
     - [System Tables Scanning (`tables.c`)](#system-tables-scanning-tablesc)
     - [Boot Services Exit (`exit_boot.c`)](#boot-services-exit-exit_bootc)
   - [UI Utilities](#ui-utilities)
     - [Splash Screen (`splash.c`)](#splash-screen-splashc)
     - [Console Helpers (`utils.c`)](#console-helpers-utilsc)
4. [Build Instructions](#build-instructions)
5. [Usage](#usage)
6. [Future Extensions](#future-extensions)

---

## Overview

LufiraOS is a hobby operating system designed for x86-64 platforms. The bootloader is the first piece of code that runs after the UEFI firmware initializes the hardware. Its responsibilities include:

- Initializing the UEFI environment and console.
- Presenting a boot menu to the user (with auto-boot countdown).
- Loading the kernel image (`kernel.bin`) from the boot partition.
- Gathering system information (memory map, graphics mode, ACPI/SMBIOS tables).
- Loading a FAT image (optional) for later use by the kernel.
- Exiting UEFI boot services and jumping to the kernel entry point.

The bootloader is designed to be modular, with clear separation between user interface, hardware detection, and kernel loading logic.

---

## Bootloader Architecture

### Boot Flow

1. **Entry Point**  
   The UEFI application starts at `efi_main()` in `boot.c`. It initializes the UEFI library, sets up the console, and clears the screen.

2. **Boot Menu**  
   The user is presented with a menu offering three boot modes: Normal, Debug, and Safe. A 5-second countdown auto-selects Normal mode if no key is pressed.

3. **Mode Selection**  
   Based on the user’s choice (or timeout), the corresponding boot function is called:
   - `QuickBoot()` for Normal mode.
   - `DebugBoot()` for Debug mode.
   - `SafeBoot()` for Safe mode.

4. **Kernel Loading**  
   All modes load the kernel from `kernel.bin` on the same volume as the bootloader. The file is read into memory at a fixed physical address (default `0x100000`).

5. **System Discovery**  
   The bootloader collects:
   - UEFI memory map.
   - Graphics output protocol information (framebuffer address, resolution, pixel format).
   - ACPI and SMBIOS table addresses.

6. **FAT Image Loading**  
   An optional FAT image (representing a disk or filesystem) can be loaded from the boot device. This is used by the kernel for filesystem access.

7. **Exit Boot Services**  
   Before handing control to the kernel, the bootloader calls `ExitBootServices()` to relinquish UEFI runtime control. The memory map is finalised and passed to the kernel.

8. **Kernel Entry**  
   The kernel is invoked as a function pointer with the `BootInfo` structure as its sole argument. After that, the bootloader no longer runs.

### Boot Modes

| Mode | Description |
|---|---|
| **Normal** | Default mode. Displays a splash screen with an animated spinner. QuickBoot loads the kernel, gathers system data, and boots with a clean UI. |
| **Debug** | Verbose mode. Displays detailed logs, memory map dump, configuration tables, and graphics information. Pauses for user confirmation before exiting boot services. |
| **Safe** | Minimal mode. Skips most UI and optional components (e.g., splash animation, FAT image errors are ignored). Used for troubleshooting. |

### Boot Information Structure (`BootInfo`)

The `BootInfo` structure is defined in `bootinfo.h` and carries all necessary data from the bootloader to the kernel.

```c
typedef struct {
    uint64_t FrameBufferBase;
    uint64_t FrameBufferSize;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    uint32_t PixelsPerScanLine;
    uint32_t PixelFormat;          // 1 = BGR, 0 = RGB
    uint64_t TotalMemory;
    uint64_t MemoryMapSize;
    void*   MemoryMap;
    uint32_t MemoryMapDescriptorSize;
    uint64_t KernelBase;
    uint64_t KernelSize;
    uint64_t RsdpAddress;
    uint64_t SmbiosAddress;
    uint64_t FATImageBase;
    uint64_t FATImageSize;
} BootInfo;
```

**Fields:**

- **Framebuffer**: address, size, resolution, scanline, and pixel format.
- **Memory**: total RAM and the raw UEFI memory map (descriptor list).
- **Kernel**: base address and size of loaded kernel image.
- **Tables**: physical addresses of ACPI RSDP and SMBIOS structures.
- **FAT Image**: base and size of a loaded FAT filesystem image (optional).

### UEFI Services Usage

The bootloader makes extensive use of UEFI Boot Services and Runtime Services:

- **Console I/O**: `gST->ConOut` for text output and cursor control.
- **Keyboard Input**: `gST->ConIn` for reading keystrokes.
- **Memory Allocation**: `gBS->AllocatePages()` and `gBS->AllocatePool()`.
- **Protocol Handlers**: `HandleProtocol()` to obtain file system, block I/O, and graphics output protocols.
- **File Access**: `EFI_FILE_PROTOCOL` to read the kernel binary.
- **Time**: `gRT->GetTime()` for system time (used in debug mode).
- **Variables**: `gRT->GetVariable()` to check Secure Boot status.

All UEFI calls are wrapped using `uefi_call_wrapper()` for compatibility.

---

## Bootloader Components

### Main Entry (`boot.c`)

The entry point performs:

- UEFI library initialization.
- Console clearing and colour setup.
- Display of the boot menu with instructions.
- Countdown loop (5 seconds) with key detection.
- Mode selection based on key input (N, D, S).
- Invocation of the appropriate boot function.
- Halt loop (should never be reached).

### Boot Mode Implementations

#### Normal Mode (`quick_boot.c`)

- Shows a splash screen (if enabled).
- Uses `LoadKernel()` to load the kernel with a live spinner animation.
- Calls `ReadMemoryMap()`, `InitializeGraphics()`, `ScanSystemTables()`.
- Attempts to load a FAT image; prints errors but continues.
- Calls `ExitBootServicesWrapper()` and jumps to the kernel.

#### Debug Mode (`debug_boot.c`)

- Clears screen and prints system information (firmware vendor, UEFI version, Secure Boot status, time, memory size).
- Loads kernel with detailed progress logs.
- Dumps the entire memory map (descriptor list) on user request.
- Dumps all UEFI configuration tables (GUID and address).
- Displays graphics output details.
- Waits for user confirmation before exiting boot services and booting.

#### Safe Mode (`safe_boot.c`)

- Minimalistic output (just “Safe mode” header).
- Loads kernel without animation.
- Collects mandatory data (memory, graphics, tables).
- Attempts FAT image loading silently; failures are ignored.
- Exits boot services and boots immediately.

### Loaders

#### Kernel Loader (`kernel_loader.c`)

- Locates the boot volume via `LoadedImage` and `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL`.
- Opens `kernel.bin` and retrieves its size.
- Allocates memory at physical address `0x100000` (if available, otherwise fails).
- Reads the file in chunks (64 KiB) and copies to the allocated buffer.
- Supports an optional spinner animation.

#### FAT Image Loader (`fat_loader.c`)

- Uses `EFI_BLOCK_IO_PROTOCOL` to read the entire boot device (or first partition) into memory.
- Limits the copy to 256 MiB (configurable).
- Allocates memory for the image and copies blocks in chunks.
- Returns the base address and size via `BootInfo`.
- Helper `GetBlockIO()` locates the block I/O protocol from the loaded image’s device handle.

### System Services

#### Memory Management (`memory.c`)

- `ReadMemoryMap()` calls `GetMemoryMap()` twice (with buffer sizing) to obtain the complete memory map.
- Stores the map, its size, and descriptor size in `BootInfo`.
- `CalculateTotalRAM()` sums pages of usable types (`EfiConventionalMemory`, `EfiLoaderCode`, `EfiLoaderData`).

#### Graphics Initialization (`graphics.c`)

- Locates the `EFI_GRAPHICS_OUTPUT_PROTOCOL`.
- Retrieves the current mode’s framebuffer base, size, resolution, scanline, and pixel format.
- Stores these in `BootInfo`.

#### System Tables Scanning (`tables.c`)

- Iterates over `gST->ConfigurationTable`.
- Detects ACPI 1.0/2.0 and SMBIOS/SMBIOS 3.0 tables by GUID.
- Saves the vendor table addresses in `BootInfo`.

#### Boot Services Exit (`exit_boot.c`)

- `ExitBootServicesWrapper()` attempts to call `ExitBootServices()`.
- If the call fails (due to an outdated memory map), it re-queries the map with extra space and retries.
- Ensures the memory map in `BootInfo` is the final one used by the kernel.

### UI Utilities

#### Splash Screen (`splash.c`)

- `ShowSplash()` draws a fixed ASCII logo in the centre of the screen.
- Displays a spinning animation for 2 seconds.
- In Debug mode, appends “[DEBUG MODE]” below the logo.

#### Console Helpers (`utils.c`)

- `SetColor()`: changes text foreground/background using UEFI colour attributes.
- `PrintColored()`: prints a string with a given colour.
- `GetConsoleSize()`: queries the current console dimensions.
- `PrintCentered()`: prints a string centred horizontally at a given row with a specified colour.

Colour constants are defined in `utils.h` (e.g., `COLOR_NEON_PINK`, `COLOR_NEON_CYAN`).

---

## Build Instructions

The bootloader is built as a UEFI application using the GNU-EFI toolchain. Ensure you have the following installed:

- `gcc` (x86_64-elf or native)
- GNU-EFI headers and libraries
- `make`

A typical build process:

```bash
# Set up environment variables (example)
export EFI_INCLUDE=/usr/include/efi
export EFI_LIB=/usr/lib

# Compile
make
```

The Makefile should compile all `.c` files and link them against the GNU-EFI libraries. The resulting `.efi` file can be placed on a FAT32 partition (ESP) and booted via UEFI firmware.

---

## Usage

1. **Prepare the boot media**
   - Format a USB drive or disk as GPT with an EFI System Partition (ESP) formatted as FAT32.
   - Copy `bootloader.efi` to `EFI/BOOT/BOOTX64.EFI` (or a custom path).
   - Place `kernel.bin` in the root of the ESP or the same directory.

2. **Boot the system**
   - Select the UEFI boot entry for your media.
   - The boot menu appears:

```text
+=============================================================+
|         LufiraOS Boot Mode Selection                       |
+=============================================================+

    [N] Normal Mode - Simple boot with animation
    [D] Debug Mode  - Detailed system information
    [S] Safe Mode   - Minimal safe boot

    Press N, D or S to select...
```

3. **Observe boot progress**
   - In Normal mode, you’ll see a splash screen and a spinner while the kernel loads.
   - In Debug mode, you’ll see extensive logs and can inspect memory maps and tables.
   - In Safe mode, only essential messages are shown.

4. **Kernel handover**

   Once the bootloader exits boot services, the kernel begins execution at its entry point with the `BootInfo` parameter.

---

## Conclusion

The LufiraOS bootloader is a robust and modular foundation for launching the operating system. Its support for multiple boot modes, detailed diagnostics, and clean separation of concerns makes it easy to extend and debug. The `BootInfo` structure provides the kernel with all the information it needs to initialise its own subsystems.

For more information, please refer to the source code and comments. Contributions and feedback are welcome!

---
