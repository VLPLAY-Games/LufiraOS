# LufiraOS

![Version](https://img.shields.io/badge/version-0.3.0-blue)
![License](https://img.shields.io/badge/license-GPL--3.0-green)
![Status](https://img.shields.io/badge/status-alpha-orange)

**LufiraOS** is a 64-bit hobby operating system for the x86_64 architecture, written from scratch in C and assembly by a single developer with some assistance from AI tools. It is designed to be educational, modular, and extensible, with a focus on understanding the core concepts of operating system development.

# 🚨 VERSION 0.3.0 (ALPHA) 🚨

> ## ⚠️ IMPORTANT NOTICE
> ### This is a **PRE-ALPHA** hobby operating system.
> ### It contains **MANY BUGS**, incomplete features, and rough edges.
> ### It is **NOT** intended for production use or daily driving.
> ### The system is a work in progress, and many features are either partially implemented or not yet functional.
> ### Use at your own risk, and expect crashes, instability, and missing functionality.
> ### See [Known Issues and Limitations](#known-issues-and-limitations) for the most significant current problems.

# ⚠️ Documentation Notice

> This documentation is provided for LufiraOS v0.3.0 and may contain inaccuracies, outdated information, or minor inconsistencies with the current source code. LufiraOS is an actively developed project, and its architecture and implementation may change over time.
>If a discrepancy exists between this documentation and the source code, the source code should be considered the authoritative reference.
> Documentation will be continuously reviewed and updated as the project evolves. See [CHANGELOG.md](CHANGELOG.md) for a detailed history of changes.

## System Requirements

| Component | Minimum Requirement |
|-----------|---------------------|
| **Architecture** | x86_64 (64-bit) |
| **RAM** | 64 MB |
| **Disk Space** | ~512 KB (kernel + bootloader) |
| **Firmware** | UEFI (BIOS/Legacy not supported) |
| **Display** | Any VESA/VBE-compatible framebuffer |
| **Audio (optional)** | AC'97 compatible audio controller |

**Note:** The system runs primarily in QEMU and may not work correctly on real hardware.

---

## Screenshots

### Bootloader Menu

![Bootloader Menu](documentation/screenshots/bootloader.jpg)

### Kernel Boot Process

![Kernel Boot](documentation/screenshots/boot.jpg)

### Interactive Shell

![Shell](documentation/screenshots/shell.jpg)

### Filesystem Operations

![Filesystem](documentation/screenshots/filesystem.jpg)

### Debug Mode

![Debug Memory Map](documentation/screenshots/debug.jpg)

### Running ELF Programs

![ELF Programs](documentation/screenshots/elf.jpg)

## Table of Contents

1. [Overview](#overview)
2. [Screenshots](#screenshots)
3. [Features](#features)
4. [Architecture Overview](#architecture-overview)
5. [Getting Started](#getting-started)
   - [Prerequisites](#prerequisites)
   - [Building](#building)
   - [Running](#running)
6. [Documentation](#documentation)
7. [Project Structure](#project-structure)
8. [Known Issues and Limitations](#known-issues-and-limitations)
9. [Contributing](#contributing)
10. [License](#license)

---

## Overview

LufiraOS is a from-scratch operating system that boots via UEFI, features a graphical console, uses its own **LufiraFS** filesystem, and provides a multitasking environment with system calls, USB/PS-2 input, and a user shell. It serves as a learning platform for OS development and a foundation for further experimentation.

### Key Concepts

- **Monolithic Kernel** – all core services (memory management, process scheduling, drivers) run in kernel space.
- **UEFI Boot** – boots on modern hardware using the UEFI firmware.
- **Graphical Console** – uses the framebuffer for text output with a custom 8×8 font, 256-color palette, and a scaled/tilted big-text renderer used for the boot logo.
- **Cooperative Multitasking** – simple round-robin scheduler with process states (READY, RUNNING, BLOCKED, SLEEPING, STOPPED, TERMINATED), `fork()`/`exec()`/`wait()`, and POSIX-style signals.
- **ELF Executable Support** – loads and runs 64-bit ELF programs.
- **System Calls** – provides a controlled interface for user-mode programs.
- **Developer Mode** – a persistent on-disk flag that switches between a quiet boot (with a logo) and a fully verbose diagnostic log; see [`04_logging.md`](documentation/04_logging.md).

---

## Features

### Bootloader

- UEFI application with three boot modes:
  - **Normal** – animated splash screen, fast boot.
  - **Debug** – detailed system information, memory map dumps, table listings.
  - **Safe** – minimal mode for troubleshooting.
- Gathers system information (memory map, framebuffer, ACPI/SMBIOS).
- Loads kernel and optional FAT image.

### Kernel

- **Memory Management**
  - Physical Memory Manager (PMM) with bitmap allocation.
  - 4-level paging (PML4, PDPT, PD, PT) with 2 MiB huge pages for identity mapping.
  - Kernel heap (16 MiB) with first-fit allocator.

- **Interrupt Handling**
  - GDT, IDT, and TSS for protected mode and user-mode transitions.
  - PIC remapping (IRQs 0–15 → vectors 32–47).
  - Exception handling with register dumps.

- **Process Management**
  - ELF loader (ET_EXEC and ET_DYN).
  - Process creation, scheduling, and termination.
  - Cooperative multitasking with timer ticks (100 Hz).
  - `fork()`, in-place `exec()`, `wait()`, and POSIX-style signals (`SIGTERM`, `SIGKILL`, `SIGSTOP`, `SIGCONT`) via `kill`.
  - Anonymous pipes for inter-process communication.
  - `syscall` instruction for fast system calls.

- **System Calls (19 implemented)**
  - File operations: `open`, `close`, `read`, `write`, `seek`, `pipe`.
  - Process control: `exit`, `getpid`, `sleep`, `kill`, `yield`, `fork`, `exec`, `wait`.
  - System info: `gettick`.
  - Stubs for `mmap`, `munmap`, `getcwd`, `chdir`.

- **Filesystem — LufiraFS**
  - Custom filesystem (superblock, block bitmap, fixed inode table, real `.`/`..` directory entries) that replaces FAT as the primary storage backend.
  - A small FAT12 partition (the UEFI ESP) still holds only the bootloader and kernel binary, since UEFI firmware can only read FAT — everything else lives on LufiraFS.
  - Virtual Filesystem (VFS) abstraction layer.
  - Dirty block tracking and flushing back to disk.
  - Full path resolution (multi-level directories, per-command cwd) and directory operations (`mkdir`, `rm`, `opendir`, `readdir`).
  - Host-side `mkfs_lufirafs` tool for formatting/populating the disk image at build time.

- **Drivers**
  - **Console** – graphical text output, 256-color palette, scrollback, scaled/tilted big-text rendering (boot logo, shell watermark).
  - **Disk (ATA PIO)** – sector read/write for primary IDE channel.
  - **Keyboard (PS/2)** – scancode translation, modifiers, IRQ1.
  - **Mouse (PS/2)** – packet decoding, IRQ12.
  - **USB (UHCI)** – host-controller driver with a USB HID boot-protocol keyboard/mouse driver, unified with PS/2 through a common input dispatcher.
  - **PCI** – bus enumeration, BAR management.
  - **AC’97 Audio** – mixer control, DMA playback, tone generation.

- **ACPI**
  - RSDP parsing (revision 1 and 2).
  - FADT detection and ACPI mode enabling.
  - System shutdown (S5 state).

- **Developer Mode & Logging**
  - Persistent on-disk flag (`/system/devmode.flag`) toggled with the `devmode` command, gating verbose boot/driver diagnostics.
  - Lightweight `klog` logger writes short status lines to `/logs/system.log` regardless of developer mode.

- **Shell**
  - Command-line interface with line editing.
  - Command history (20 entries).
  - Tab completion (command names).
  - Built-in commands: system control, file management (including `df`/`du`/colourised `ls`), process control, signals, audio, developer mode.
  - Current working directory (cwd) support.

---

## Architecture Overview

```svg

+--------------------------------------------------+
\| USER MODE |
\| +------------------------------------------+ |
\| | Shell / User Programs | |
\| | (ELF executables) | |
\| +------------------------------------------+ |
\| | |
\| syscall |
\| | |
+--------------------------------------------------+
\| KERNEL MODE |
\| +------------------------------------------+ |
\| | System Calls (19) | |
\| +------------------------------------------+ |
\| | VFS / LufiraFS Driver | |
\| +------------------------------------------+ |
\| | Process Scheduler / ELF Loader | |
\| | (fork / exec / wait / signals / pipes) | |
\| +------------------------------------------+ |
\| | Memory Management (PMM / Paging / Heap) | |
\| +------------------------------------------+ |
\| | Drivers (Console, Disk, Keyboard, Mouse, | |
\| | USB/UHCI, PCI, AC'97, ACPI) | |
\| +------------------------------------------+ |
\| | Devmode / klog | |
\| +------------------------------------------+ |
\| | CPU / Interrupts (GDT, IDT, IRQ, TSS) | |
\| +------------------------------------------+ |
\| | |
+----------------------+-----------------------------+
|
UEFI Bootloader
|
BootInfo Structure

````
---

## Getting Started

### Prerequisites

- **GCC** (x86_64-elf or with cross-compile support)
- **GNU ld**, **objcopy**, **nm**
- **GNU-EFI** headers and libraries (`/usr/include/efi`, `/usr/lib`)
- **dosfstools** (`mkfs.fat`)
- **mtools** (`mmd`, `mcopy`)
- **QEMU** with OVMF firmware
- **make**

### Building

```bash
# Clone the repository
git clone https://github.com/yourusername/lufiraos.git
cd lufiraos

# Build everything and run
make clean && make run
````

### Running



``` bash
# Run in QEMU (quiet boot, boot logo, developer mode off)
make run

# Run with developer mode enabled automatically (verbose boot/driver log,
# see documentation/04_logging.md) plus QEMU's own debug logging
make debug

# Run with QEMU monitor (telnet on port 4444)
make monitor

# Clean build artefacts
make clean
```

**QEMU Parameters:**

- OVMF UEFI firmware (`/usr/share/ovmf/OVMF.fd`)
- Disk image as IDE drive
- 128 MB RAM
- AC’97 audio (ALSA backend)
- Serial output redirected to stdio

---

## Documentation

Detailed documentation is available in the `documentation/` directory:

| **FileDescription**                                                        |                                                             |
| -------------------------------------------------------------------------- | ----------------------------------------------------------- |
| [`01_bootloader.md`](https://documentation/01_bootloader.md)               | UEFI bootloader, boot modes, BootInfo structure             |
| [`02_kernel_init.md`](https://documentation/02_kernel_init.md)             | Kernel entry point and initialization order                 |
| [`03_bootinfo.md`](https://documentation/03_bootinfo.md)                   | BootInfo structure reference                                |
| [`04_logging.md`](https://documentation/04_logging.md)                     | Logging macros (log.h)                                      |
| [`05_build_system.md`](https://documentation/05_build_system.md)           | Build system and QEMU usage                                 |
| [`06_libraries.md`](https://documentation/06_libraries.md)                 | System libraries (types, colors, string, etc.)              |
| [`07_drivers.md`](https://documentation/07_drivers.md)                     | Device drivers (console, disk, keyboard, mouse, PCI, AC'97) |
| [`08_filesystem.md`](https://documentation/08_filesystem.md)               | FAT driver and Virtual Filesystem (VFS)                     |
| [`09_acpi.md`](https://documentation/09_acpi.md)                           | ACPI subsystem (RSDP, FADT, shutdown)                       |
| [`10_cpu_interrupts.md`](https://documentation/10_cpu_interrupts.md)       | CPU, GDT, IDT, IRQ, TSS, PIT                                |
| [`11_memory_management.md`](https://documentation/11_memory_management.md) | PMM, Paging, Heap                                           |
| [`12_elf_processes.md`](https://documentation/12_elf_processes.md)         | ELF loader and process management                           |
| [`13_syscalls.md`](https://documentation/13_syscalls.md)                   | System calls (syscall instruction, table, handler)          |
| [`14_shell_commands.md`](https://documentation/14_shell_commands.md)       | Shell and built-in commands                                 |

---

## Project Structure

text

```
lufiraos/
├── boot/                      # UEFI bootloader sources
│   ├── boot.c                 # Entry point, menu, countdown
│   ├── boot_modes/            # Boot mode implementations
│   │   ├── quick_boot.c       # Normal mode
│   │   ├── debug_boot.c       # Debug mode
│   │   └── safe_boot.c        # Safe mode
│   ├── loaders/               # Loaders
│   │   ├── kernel_loader.c    # Kernel loader
│   │   └── fat_loader.c       # FAT image loader
│   ├── system/                # System services
│   │   ├── memory.c           # Memory map
│   │   ├── graphics.c         # Graphics initialization
│   │   ├── tables.c           # ACPI/SMBIOS tables
│   │   └── exit_boot.c        # ExitBootServices
│   ├── ui/                    # UI utilities
│   │   ├── splash.c           # Splash screen
│   │   └── utils.c            # Console helpers
│   └── bootinfo.h             # BootInfo structure
│
├── kernel/                    # Kernel sources
│   ├── kernel.c               # Entry point, initialization
│   ├── linker.ld              # Linker script
│   ├── drivers/               # Device drivers
│   │   ├── console/           # Console driver
│   │   ├── disk/              # ATA PIO driver
│   │   ├── keyboard/          # PS/2 keyboard driver
│   │   ├── mouse/             # PS/2 mouse driver
│   │   ├── usb/               # UHCI host controller + USB HID driver
│   │   ├── input/             # Shared PS/2 + USB HID input dispatcher
│   │   ├── pci/               # PCI bus driver
│   │   └── sound/             # AC'97 audio driver
│   ├── fs/                    # Filesystem
│   │   ├── lufirafs/          # LufiraFS driver (primary filesystem)
│   │   │   ├── lufirafs.c            # Core implementation
│   │   │   ├── lufirafs_vfs.c        # VFS wrapper
│   │   │   ├── lufirafs.h            # Driver API
│   │   │   └── lufirafs_format.h     # On-disk format (shared with mkfs_lufirafs)
│   │   ├── fat/                # Legacy FAT driver (kept for reference, unused)
│   │   └── vfs/                # Virtual Filesystem
│   │       ├── vfs.c          # VFS core
│   │       └── vfs.h          # VFS header
│   ├── lib/                   # System libraries
│   │   ├── types.h            # Basic types
│   │   ├── colors.h           # Color definitions
│   │   ├── string.c/h         # String utilities
│   │   ├── cpu.c/h            # CPU utilities
│   │   └── stdarg.h           # Variable arguments
│   ├── shell/                 # Shell
│   │   ├── shell.c            # Shell core
│   │   ├── shell.h            # Shell header
│   │   └── commands/          # Built-in commands
│   │       ├── system.c       # System commands (incl. devmode)
│   │       ├── colors.c       # Color commands
│   │       ├── filesystem.c   # Filesystem commands
│   │       └── sound.c        # Audio commands
│   └── system/                # Kernel subsystems
│       ├── acpi/              # ACPI
│       ├── cpu/               # CPU management (GDT, IDT, IRQ, TSS)
│       ├── devmode/           # Developer-mode flag (persistent, gates DLOG)
│       ├── klog/              # Persistent event logging (/logs/system.log)
│       ├── elf/               # ELF loader
│       ├── mm/                # Memory management (PMM, Paging, Heap)
│       ├── process/           # Process management (incl. fork/exec/signals)
│       ├── syscall/           # System calls
│       └── timer/             # PIT timer
│
├── tools/                     # Host-side build tools
│   └── mkfs_lufirafs.c        # LufiraFS formatting/populating tool
│
├── build/                     # Build artefacts (created by make)
│   ├── BOOTX64.EFI            # UEFI bootloader
│   ├── kernel.bin             # Kernel binary
│   ├── kernel.elf             # Kernel with debug symbols
│   ├── mkfs_lufirafs          # Host tool binary
│   └── disk.img               # Complete disk image (ESP + LufiraFS region)
│
├── Makefile                   # Build system
├── README.md                  # This file
├── CHANGELOG.md               # Version history
└── documentation/             # Documentation
    ├── 01_bootloader.md
    ├── 02_kernel_init.md
    └── ...
```

---

## Known Issues and Limitations

- **Keyboard auto-repeat does not work.** Holding down a key registers as a single keypress instead of repeating.
- **`fork()`/`exec()` are unreliable.** A forked child process can crash (triple fault) before it ever reaches its first instruction; the root cause has not yet been isolated. Treat both as experimental.
- **No preemptive multitasking.** Scheduling is strictly cooperative; a process that never yields (via a syscall, sleep, or the timer's implicit halt/schedule cycle) can stall the rest of the system.
- **No memory protection beyond paging permissions.** `mmap`/`munmap`/`getcwd`/`chdir` are still stubs.
- `cd`, `cp`, and `mv` are not fully stable — will be fixed in the next release (v0.3.1).
- **Real hardware is untested.** The system is developed and tested exclusively in QEMU; UEFI/ACPI/USB quirks on real firmware are unknown.
- See [CHANGELOG.md](CHANGELOG.md) for issues fixed in past versions.

---

## Contributing

Contributions are welcome! Here are some areas for improvement:

- **Filesystem**: Long file name (LFN) support, additional filesystem drivers (ext2, ISO9660), LufiraFS indirect-block-chain growth beyond a single indirect block.
- **Drivers**: AHCI/SATA, network, graphics acceleration, USB mass storage.
- **Processes**: Preemptive multitasking, a working `fork()`/`exec()`, copy-on-write address spaces.
- **System Calls**: Implement remaining stubs (`mmap`, `munmap`, `getcwd`, `chdir`).
- **Shell**: Redirection, environment variables, scripting.
- **Security**: Memory protection, user/kernel separation, paging permissions.
- **Documentation**: More examples, tutorials, API references.

### Guidelines

1. Follow the existing code style (K&R with 4-space indentation).
2. Keep functions modular and well-commented.
3. Update documentation when adding new features.
4. Test changes in QEMU before submitting.

---

## License

This project is licensed under the GPL-3.0 License. See the [LICENSE](LICENSE) file for details.

---

---

**LufiraOS** – Building an OS from scratch, one commit at a time.