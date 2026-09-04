```
# Build System

This document describes the build system used to compile the LufiraOS bootloader, kernel, and disk image. The build is managed by a comprehensive Makefile that handles all build steps, dependency checking, and QEMU execution.

---

## Table of Contents

1. [Overview](#overview)
2. [Prerequisites](#prerequisites)
3. [Build Commands](#build-commands)
4. [Build Outputs](#build-outputs)
5. [Build Process](#build-process)
   - [Bootloader Compilation](#bootloader-compilation)
   - [Kernel Compilation](#kernel-compilation)
   - [Disk Image Creation](#disk-image-creation)
6. [Running in QEMU](#running-in-qemu)
   - [Standard Run](#standard-run)
   - [Debug Mode](#debug-mode)
   - [Monitor Mode](#monitor-mode)
7. [Adding Files to the Disk Image](#adding-files-to-the-disk-image)
8. [Directory Structure](#directory-structure)
9. [Troubleshooting](#troubleshooting)
10. [Future Extensions](#future-extensions)

---

## Overview

The LufiraOS build system is designed to be simple, fast, and self-contained. It uses GNU Make and standard Unix tools to:

- Compile the UEFI bootloader using the GNU-EFI framework.
- Compile the kernel using GCC with custom flags.
- Link the kernel using a custom linker script.
- Create a bootable disk image with a FAT filesystem.
- Launch the system in QEMU for testing and debugging.

**Key Features:**
- **Modular Build** – bootloader and kernel can be built separately.
- **Automatic Dependency Checking** – required tools are verified before building.
- **Clean Separation** – source, build artefacts, and outputs are kept separate.
- **QEMU Integration** – the system can be launched directly from the Makefile.
- **Debugging Support** – special targets for verbose logging and QEMU monitor.

---

## Prerequisites

### Required Tools

| Tool | Purpose | Package (Ubuntu/Debian) |
|------|---------|-------------------------|
| `gcc` | C compiler | `gcc` |
| `ld` | Linker | `binutils` |
| `objcopy` | Binary conversion | `binutils` |
| `nm` | Symbol listing | `binutils` |
| `make` | Build automation | `make` |
| `truncate` | File size manipulation | `coreutils` |
| `dd` | Raw disk writing | `coreutils` |
| `mkfs.fat` | FAT filesystem creation | `dosfstools` |
| `mmd` | Create FAT directory | `mtools` |
| `mcopy` | Copy to FAT image | `mtools` |
| `qemu-system-x86_64` | Emulator | `qemu-system-x86` |
| OVMF firmware | UEFI boot in QEMU | `ovmf` or `edk2-ovmf` |

### Environment Setup

The Makefile assumes the following default paths:
- GNU-EFI headers: `/usr/include/efi`
- GNU-EFI libraries: `/usr/lib`
- OVMF firmware: `/usr/share/ovmf/OVMF.fd`

**Note:** On some distributions, OVMF may be located in `/usr/share/edk2-ovmf/x64/OVMF.fd` or similar. You may need to adjust the `-bios` path in the `run` target.

---

## Build Commands

| Command | Description |
|---------|-------------|
| `make` or `make all` | Builds the complete disk image (bootloader + kernel). |
| `make bootloader` | Builds only the UEFI bootloader (`BOOTX64.EFI`). |
| `make kernel` | Builds only the kernel binary (`kernel.bin`). |
| `make disk` | Creates the disk image (requires bootloader and kernel). |
| `make clean` | Removes all build artefacts. |
| `make run` | Launches QEMU with the disk image. |
| `make debug` | Launches QEMU with debug logging enabled. |
| `make monitor` | Launches QEMU with a telnet monitor. |
| `make check-disk` | Lists the contents of the disk image. |
| `make info` | Shows build configuration and file lists. |
| `make quick` | Cleans and rebuilds everything from scratch. |

---

## Build Outputs

All build artefacts are placed in the `build/` directory:

| File | Description |
|------|-------------|
| `BOOTX64.EFI` | UEFI bootloader binary. |
| `kernel.bin` | Raw kernel binary (for bootloader to load). |
| `kernel.elf` | Kernel ELF file with debug symbols. |
| `disk.img` | Complete bootable disk image (FAT12 formatted). |
| `*.o` | Object files for each source file. |

**Build Directory Structure:**
```


build/
├── boot/ # Bootloader object files
│ ├── boot.o
│ ├── boot\_modes/
│ ├── loaders/
│ ├── system/
│ └── ui/
├── kernel/ # Kernel object files
│ ├── drivers/
│ ├── fs/
│ ├── lib/
│ ├── shell/
│ └── system/
├── BOOTX64.EFI # UEFI bootloader
├── kernel.bin # Kernel binary (stripped)
├── kernel.elf # Kernel with debug symbols
└── disk.img # Complete disk image


```
---

## Build Process

### Bootloader Compilation

1. **Compile each C file** with GNU-EFI flags:
   - Position-independent code (`-fpic`)
   - Freestanding environment (`-ffreestanding`)
   - No stack protection (`-fno-stack-protector`)
   - Short wchar support (`-fshort-wchar`)
   - No red zone (`-mno-red-zone`)
   - GNU C11 standard (`-std=gnu11`)

2. **Link the object files** with GNU-EFI libraries:
   - Uses the EFI linker script (`elf_x86_64_efi.lds`)
   - Shared library format (`-shared`, `-Bsymbolic`)
   - Links against `-lefi` and `-lgnuefi`

3. **Convert to EFI binary** using `objcopy`:
   - Targets `efi-app-x86_64`
   - Selects only relevant sections (`.text`, `.data`, `.dynamic`, `.reloc`, etc.)

### Kernel Compilation

1. **Compile C sources** with kernel flags:
   - 64-bit target (`-m64`)
   - Freestanding (`-ffreestanding`)
   - No stack protection or checking
   - No built-in functions (`-fno-builtin`)
   - No red zone (`-mno-red-zone`)
   - General-purpose registers only (`-mgeneral-regs-only`)
   - GNU C11 standard (`-std=gnu11`)

2. **Compile assembly sources** with preprocessor support (`-x assembler-with-cpp`).

3. **Link the object files** with the custom linker script (`linker.ld`):
   - Static linking (`-static`)
   - No standard libraries (`-nostdlib`)
   - Custom page alignment (`-z max-page-size=0x1000`)
   - Dead code elimination (`--gc-sections`)

4. **Extract the binary** using `objcopy`:
   - Raw binary output (`-O binary`)
   - The linker script defines `__kernel_end` symbol for size calculation.

5. **Calculate kernel size** from the `__kernel_end` symbol using `nm` and `truncate`.

### Disk Image Creation

1. **Create an empty disk image** using `dd` (512 KiB initially).

2. **Format as FAT12** using `mkfs.fat`:
   - File Allocation Table: 12-bit (`-F 12`)
   - Sector size: 512 bytes (`-S 512`)

3. **Create directories** using `mmd`:
   - `:/EFI`
   - `:/EFI/BOOT`

4. **Copy the bootloader** using `mcopy`:
   - `build/BOOTX64.EFI` → `:/EFI/BOOT/BOOTX64.EFI`

5. **Copy the kernel** using `mcopy`:
   - `build/kernel.bin` → `:/kernel.bin`

6. **Create additional directories and test files**:
   - `/test` directory
   - `/readme.txt` with a greeting message

---

## Running in QEMU

### Standard Run

```bash
make run
```


**QEMU Parameters:**

- **BIOS:** OVMF UEFI firmware (`/usr/share/ovmf/OVMF.fd`)
- **Disk:** `build/disk.img` as IDE drive (raw format, index 0)
- **Memory:** 128 MB (`-m 128M`)
- **Network:** Disabled (`-net none`)
- **Audio:** PC speaker and AC'97 audio (`-machine pcspk-audiodev=audio`, `-audiodev driver=alsa,id=audio`, `-device AC97,audiodev=audio`)
- **Output:** Serial port redirected to stdio (`-serial stdio`)

**Run Customisations:**

- Adjust memory size: `-m 256M`
- Disable audio: remove `-audiodev` and `-device AC97`
- Use different OVMF path: change `-bios` parameter

### Debug Mode


```
make debug
```


**Additional QEMU Parameters:**

- No reboot (`-no-reboot`)
- No shutdown (`-no-shutdown`)
- Debug logging: `-d cpu_reset,guest_errors`
- Log output: `build/qemu_debug.log`

**Use Case:** Useful for diagnosing early boot failures, page faults, and CPU exceptions.

### Monitor Mode


```
make monitor
```


**Additional QEMU Parameters:**

- Telnet monitor on port 4444 (`-monitor telnet:127.0.0.1:4444,server,nowait`)

**Connect to Monitor:**


```
telnet localhost 4444
```


**Useful Monitor Commands:**

- `info registers` – show CPU state
- `info mem` – show memory mapping
- `xp /x ADDR` – examine physical memory
- `stop` / `cont` – pause/resume execution
- `system_reset` – reset the emulator

---

## Adding Files to the Disk Image

### Using MTOOLS


```
mcopy -i build/disk.img hello.elf ::/hello.elf
mmd -i build/disk.img ::/newdir
```


### Using Automatic Copy with Make

The `make run` target can automatically copy files from the project root:


```
# Add to Makefile
mcopy -i build/disk.img hello.elf ::/hello.elf
```


### Checking Disk Contents


```
make check-disk
mdir -i build/disk.img ::/
mdir -i build/disk.img ::/EFI/BOOT/
```


---

## Directory Structure


```
lufiraos/
├── boot/                      # Bootloader sources
│   ├── boot.c                 # Main entry
│   ├── boot_modes/            # Boot mode implementations
│   ├── loaders/               # Kernel and FAT loaders
│   ├── system/                # System services
│   └── ui/                    # UI utilities
├── kernel/                    # Kernel sources
│   ├── kernel.c               # Kernel entry and init
│   ├── linker.ld              # Linker script
│   ├── drivers/               # Device drivers
│   ├── fs/                    # Filesystem (FAT, VFS)
│   ├── lib/                   # System libraries
│   ├── shell/                 # Shell and commands
│   └── system/                # Kernel subsystems
├── build/                     # Build artefacts (created)
│   ├── BOOTX64.EFI            # EFI bootloader
│   ├── kernel.bin             # Kernel binary
│   ├── kernel.elf             # Kernel with symbols
│   └── disk.img               # Complete disk image
├── Makefile                   # Build system
└── README.md                  # Project documentation
```


---

## Troubleshooting

### Missing Tools

**Error:** `Required tool 'xxx' not found in PATH`

**Solution:** Install the missing package:


```
# Ubuntu/Debian
sudo apt install gcc binutils make dosfstools mtools qemu-system-x86 ovmf

# Arch Linux
sudo pacman -S gcc binutils make dosfstools mtools qemu-system-x86 edk2-ovmf

# Fedora
sudo dnf install gcc binutils make dosfstools mtools qemu-system-x86 edk2-ovmf
```


### OVMF Not Found

**Error:** `Could not open ROM file /usr/share/ovmf/OVMF.fd`

**Solution:** Update the `-bios` path in the Makefile:


```
# Ubuntu/Debian
-bios /usr/share/ovmf/OVMF.fd

# Arch Linux
-bios /usr/share/edk2-ovmf/x64/OVMF.fd

# Fedora
-bios /usr/share/edk2/ovmf/OVMF.fd
```


### Build Directory Cleanup

**Error:** Stale object files causing issues.

**Solution:** Clean and rebuild:


```
make clean
make all
```


### QEMU Audio Issues

**Error:** `audiodev driver=alsa` fails.

**Solution:** Use PulseAudio instead:


```
-audiodev driver=pa,id=audio
```


Or disable audio:


```
# Remove -audiodev and -device AC97 lines
```


### Kernel Size Calculation

**Error:** `__kernel_end` symbol not found.

**Solution:** Ensure `linker.ld` defines the symbol correctly:


```
__kernel_end = .;
```


And the Makefile uses `nm` to read it:


```
KERNEL_END=$$(nm $(BUILD_DIR)/kernel.elf | awk '$$3=="__kernel_end"{print $$1}')
```


---

## Conclusion

The LufiraOS build system is designed to be simple, reliable, and easy to use. By leveraging standard Unix tools and GNU Make, it provides a consistent build experience across different systems. The integration with QEMU makes testing and debugging straightforward, and the modular structure allows developers to build only the components they need.

For more details, refer to the source code in the `Makefile` and the individual build configurations.

---

**Document Version:** 1.0
**Last Updated:** September 2026
**Project:** LufiraOS
