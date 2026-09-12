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
- Build the host-side `mkfs_lufirafs` tool and use it to format/populate the LufiraFS region of the disk image.
- Create a bootable disk image with a small FAT12 ESP (for UEFI firmware) followed by a LufiraFS region.
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
| `make run` | Builds the complete disk image (bootloader + kernel). |
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
| `mkfs_lufirafs` | Host-compiled tool for formatting/populating the LufiraFS region (see [`08_filesystem.md`](08_filesystem.md)). |
| `disk.img` | Complete bootable disk image: a FAT12 ESP followed by a LufiraFS region. |
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

The disk image (16 MiB by default, `DISK_TOTAL_SIZE`) is built in two independent stages that are then concatenated. `LUFIRAFS_ESP_SIZE` (4 MiB) must match the constant of the same name in `kernel/fs/lufirafs/lufirafs_format.h` — a mismatch means `mkfs_lufirafs` formats a different byte range than the one the kernel actually mounts.

**Stage 1 — the ESP (FAT12, read by UEFI firmware):**

1. Create an empty `build/esp.img` sized `LUFIRAFS_ESP_SIZE` using `dd`.
2. Format it as FAT12 using `mkfs.fat -F 12 -S 512`.
3. Create `::/EFI` and `::/EFI/BOOT` using `mmd`.
4. Copy `build/BOOTX64.EFI` → `::/EFI/BOOT/BOOTX64.EFI` and `build/kernel.bin` → `::/kernel.bin` using `mcopy`.
5. `dd` this ESP image into `disk.img` at offset 0 (`conv=notrunc`).

**Stage 2 — the LufiraFS region:**

6. Build `build/mkfs_lufirafs` (a normal hosted C program) from `tools/mkfs_lufirafs.c`.
7. `mkfs_lufirafs format` writes a fresh LufiraFS superblock/bitmap/inode table into the remaining `LUFIRAFS_REGION_SIZE` bytes of `disk.img`.
8. `mkfs_lufirafs mkdir`/`put` create `/test`, `/system`, `/logs`, and `/readme.txt`.

See [`08_filesystem.md`](08_filesystem.md) for the on-disk format itself.

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

Before launching QEMU, the `debug` target writes a `/system/devmode.flag` marker file into `disk.img` (via `mkfs_lufirafs put`), which enables [developer mode](04_logging.md) automatically — every `make debug` run shows the full verbose boot/driver log. This step is idempotent, so running `make debug` repeatedly against the same image is safe. `make run` does **not** touch the flag, so it always starts with whatever developer-mode state the disk image already has (off, by default, on a freshly built image).

**Use Case:** Useful for diagnosing early boot failures, page faults, CPU exceptions, and driver issues that developer mode's verbose log would otherwise hide.

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

**Important:** `mtools` (`mcopy`/`mmd`) only understands the FAT12 ESP region — it cannot see or modify the LufiraFS region at all. Since the ESP is meant to hold only `/EFI/BOOT/BOOTX64.EFI` and `/kernel.bin` (see [`08_filesystem.md`](08_filesystem.md)), use `mkfs_lufirafs` to add anything else to the disk image.

### Using `mkfs_lufirafs`

```bash
build/mkfs_lufirafs put   build/disk.img $(LUFIRAFS_ESP_SIZE) $(LUFIRAFS_REGION_SIZE) hello.elf /hello.elf
build/mkfs_lufirafs mkdir build/disk.img $(LUFIRAFS_ESP_SIZE) $(LUFIRAFS_REGION_SIZE) /newdir
```

`put` creates parent directories as needed; `mkdir` behaves like `mkdir -p`.

### Using Automatic Copy with Make

The `run` target already does this for the bundled test programs — see the `$(BUILD_DIR)/mkfs_lufirafs put ...` lines right before the `qemu-system-x86_64` invocation in the Makefile. Add another line there, following the same pattern, to bundle additional files on every `make run`.

### Checking Disk Contents

```bash
make check-disk        # runs `file build/disk.img` — confirms it is a valid disk image
```

There is currently no `mkfs_lufirafs` subcommand to list a directory's contents from the host side — boot the image and use the shell's `ls`/`cat` commands instead.

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
│   ├── drivers/               # Device drivers (incl. usb/, input/)
│   ├── fs/                    # Filesystem (lufirafs/, vfs/, legacy fat/)
│   ├── lib/                   # System libraries
│   ├── shell/                 # Shell and commands
│   └── system/                # Kernel subsystems (incl. devmode/, klog/)
├── tools/                     # Host-side build tools
│   └── mkfs_lufirafs.c        # LufiraFS formatting/populating tool
├── build/                     # Build artefacts (created)
│   ├── BOOTX64.EFI            # EFI bootloader
│   ├── kernel.bin             # Kernel binary
│   ├── kernel.elf             # Kernel with symbols
│   ├── mkfs_lufirafs          # Host tool binary
│   └── disk.img               # Complete disk image (ESP + LufiraFS)
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
make run
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
