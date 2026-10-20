# LufiraOS Bootloader Documentation

Welcome to the LufiraOS project! This document provides a comprehensive overview of the bootloader subsystem, its architecture, components, and usage. The bootloader is a UEFI application written in C that prepares the system environment, loads the kernel, and transfers control to it. It supports multiple boot modes, graphical splash screens, and detailed diagnostics.

**Scope note:** everything below, up to [Legacy BIOS Boot](#legacy-bios-boot-boot_legacy), describes the **UEFI boot path only** (the `boot/` directory — `boot.c`, `quick_boot.c`/`debug_boot.c`/`safe_boot.c`, `kernel_loader.c`, etc.). Since v0.9.0 there is a second, independent boot path for firmware without UEFI support — **legacy BIOS boot** (`boot_legacy/`) — covered in its own section near the end of this document. Both paths produce the exact same `BootInfo` structure and hand off to the exact same `kernel.bin`; the kernel itself has no idea which path booted it.

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
6. [Legacy BIOS Boot (`boot_legacy/`)](#legacy-bios-boot-boot_legacy)
7. [Future Extensions](#future-extensions)

---

## Overview

LufiraOS is a hobby operating system designed for x86-64 platforms. The bootloader is the first piece of code that runs after the UEFI firmware initializes the hardware. Its responsibilities include:

- Initializing the UEFI environment and console.
- Presenting a boot menu to the user (with auto-boot countdown).
- Loading the kernel image (`kernel.bin`) from the boot partition.
- Gathering system information (memory map, graphics mode, ACPI/SMBIOS tables).
- Loading the entire raw boot disk into memory as one image (optional) for later use by the kernel — as of v0.3.0 this image holds a small FAT12 ESP followed by the kernel's own LufiraFS region; the bootloader itself is unaware of this split, see `08_filesystem.md`.
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

## Legacy BIOS Boot (`boot_legacy/`)

As of v0.9.0, LufiraOS has a second boot path — **legacy BIOS boot** — alongside the UEFI path described above, for firmware that doesn't support UEFI at all. It is a separate, self-contained directory (`boot_legacy/`: `stage1.asm`, `stage2.asm`, `legacy_main.c`/`.h`, `legacy_boot.h`, `layout.inc`) that boots the **same `disk.img` and the same `kernel.bin`** the UEFI path uses — nothing in `boot/` or in the kernel changes to support it, and `LufiraOS-Builder` only embeds this chain when asked to (`--legacy-bios`, which needs `nasm`; see [`05_build_system.md`](05_build_system.md)).

### Why it exists

`disk.img`'s first region is a FAT12 "superfloppy" ESP that UEFI firmware loads `BOOTX64.EFI` from directly. That same sector 0 can instead hold an ordinary **MBR-style boot sector** that real/legacy BIOS firmware (and BIOS-compatibility modes in newer firmware) loads at `0x7C00` — which is exactly what `stage1.asm` is.

### Boot flow

1. **`stage1.asm` (boot sector, real mode).** `mkfs.fat` already writes a valid FAT12 BIOS Parameter Block into bytes `0x00`-`0x3D` of sector 0 (including the mandatory `jmp short 0x3E; nop`); `stage1.asm` supplies only the **code region** that follows it (`0x3E`-`0x1FD`, 448 bytes, patched in by `LufiraOS-Builder`'s `lufira_builder/legacy_boot.py` — the BPB and the `0x55AA` signature are left untouched). It normalizes `CS`/`DS`/`ES`/`SS`, checks for INT13h extensions (LBA reads — there is no legacy CHS fallback anywhere in this path), and uses INT13h `AH=42h` to read a fixed number of sectors (`STAGE2_MAX_SECTORS`, starting at `STAGE2_LBA_START`, see `layout.inc`) into `STAGE2_LOAD_SEGMENT:0x0000`, then jumps there.
2. **`stage2.asm` (real mode → protected mode → long mode).** The larger of the two assembly stages:
   - Enables the A20 line (fast method, port `0x92`).
   - Builds a temporary "unreal mode" for `ES` (a GDT trick) so 16-bit code can write to physical memory above 1 MiB without re-entering protected mode for every access.
   - Calls `do_e820` (memory map, same purpose as the UEFI path's `GetMemoryMap()`) and `do_vesa` (finds a linear-framebuffer VESA VBE mode — 1024x768/800x600/etc. by priority — same purpose as the UEFI path's Graphics Output Protocol query).
   - Loads `legacy_main.bin`, the kernel (`kernel.bin`, to physical `0x100000` — the same address the UEFI loader uses) and the whole raw disk image, the same three payloads `boot/`'s loaders stage for the kernel.
   - Builds a flat GDT (null/data32/code32/code64 descriptors) and performs the real → protected (32-bit) → long mode transition, with an identity-mapped 0-4 GiB region using 2 MiB pages (covers both RAM and the VESA linear framebuffer, whose `PhysBasePtr` is always 32-bit).
   - Jumps into `legacy_main.bin` with `RDI` pointing at a `LegacyHandoff` structure.
3. **`legacy_main.c` (long mode, C).** Compiled and loaded as a separate flat binary, exactly like `kernel.bin` (`objcopy -O binary`, entry point = first byte). Its only job is to turn `LegacyHandoff` — the raw data a 16-bit BIOS boot path can realistically gather — into the **exact same `BootInfo` structure** the UEFI path builds (see [Boot Information Structure](#boot-information-structure-bootinfo) above), then call the kernel entry point with it:
   - Framebuffer fields copied directly from `LegacyHandoff`.
   - `MemoryMap`/`MemoryMapSize`/`TotalMemory` built by converting raw E820 entries (`convert_e820()`) into `EFI_MEMORY_DESCRIPTOR`s (E820 "usable" → `EfiConventionalMemory`, everything else → reserved) — the same descriptor layout `kernel/system/mm/pmm.c` already expects from the UEFI path.
   - `RsdpAddress`/`SmbiosAddress` found by scanning the EBDA and the `0xE0000`-`0xFFFFF` BIOS area for the `"RSD PTR "` / `"_SM3_"`/`"_SM_"` signatures — the BIOS-era equivalent of the UEFI path's configuration-table scan (`tables.c`).
   - `KernelBase`/`KernelSize`/`FATImageBase`/`FATImageSize` passed straight through from `LegacyHandoff`.

   If `KernelBase` is 0 (nothing loaded), it halts forever instead of jumping into garbage; otherwise it calls into the kernel exactly like the UEFI path does, and never expects to return.

### `LegacyHandoff` structure

The real-mode/protected-mode side (`stage2.asm`) can only gather a small, packed struct (`boot_legacy/legacy_boot.h`, `__attribute__((packed))`, statically asserted to be exactly 72 bytes) — raw E820 pointer/count, VESA framebuffer geometry, and the kernel/FAT-image base+size — which `legacy_main.c` then expands into the full `BootInfo`. Field offsets are duplicated by hand as `HANDOFF_OFF_*` constants in `stage2.asm` and must stay in sync if the struct changes.

### Known fixed bug

Legacy BIOS boot briefly had console/Doom colors slightly inverted compared to UEFI boot, caused by an off-by-one-byte read of the VESA `ModeInfoBlock` in `stage2.asm` (`RedMaskSize` at `+0x1F` instead of `RedFieldPosition` at `+0x20`) — fixed in v0.9.0.

### Building and launching

`LufiraOS-Builder`'s `--legacy-bios` flag builds this chain (`make bootloader-bios` in addition to the normal `make kernel bootloader`) and embeds it into `disk.img`'s boot sector alongside the normal UEFI path; its GUI (`gui.py`) exposes a "Legacy BIOS" launch-mode radio button that only becomes selectable once `--legacy-bios` is checked. See [`05_build_system.md`](05_build_system.md) for the full build-system picture.

---

## Conclusion

The LufiraOS bootloader is a robust and modular foundation for launching the operating system. Its support for multiple boot modes, detailed diagnostics, and clean separation of concerns makes it easy to extend and debug. The `BootInfo` structure provides the kernel with all the information it needs to initialise its own subsystems.

For more information, please refer to the source code and comments. Contributions and feedback are welcome!

---
