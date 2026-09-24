# LufiraOS

![Version](https://img.shields.io/badge/version-0.6.0-blue)
![License](https://img.shields.io/badge/license-GPL--3.0-green)
![Status](https://img.shields.io/badge/status-alpha-orange)

**LufiraOS** is a 64-bit hobby operating system for the x86_64 architecture, written from scratch in C and assembly by a single developer with some assistance from AI tools. It is designed to be educational, modular, and extensible, with a focus on understanding the core concepts of operating system development.

# 🚨 VERSION 0.6.0 (ALPHA) 🚨

> ## ⚠️ IMPORTANT NOTICE
> ### This is a **PRE-ALPHA** hobby operating system.
> ### It contains **MANY BUGS**, incomplete features, and rough edges.
> ### It is **NOT** intended for production use or daily driving.
> ### The system is a work in progress, and many features are either partially implemented or not yet functional.
> ### Use at your own risk, and expect crashes, instability, and missing functionality.
> ### See [Known Issues and Limitations](#known-issues-and-limitations) for the most significant current problems.

# ⚠️ Documentation Notice

> This documentation is provided for LufiraOS v0.6.0 and may contain inaccuracies, outdated information, or minor inconsistencies with the current source code. LufiraOS is an actively developed project, and its architecture and implementation may change over time.
>If a discrepancy exists between this documentation and the source code, the source code should be considered the authoritative reference.
> Documentation will be continuously reviewed and updated as the project evolves. See [CHANGELOG.md](CHANGELOG.md) for a detailed history of changes.

## System Requirements

| Component | Minimum Requirement |
|-----------|---------------------|
| **Architecture** | x86_64 (64-bit) |
| **RAM** | 64 MB |
| **Disk Space** | 16 MB (fixed-size disk image: FAT12 ESP + LufiraFS region) |
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

LufiraOS is a from-scratch operating system that boots via UEFI, features a graphical console, uses its own **LufiraFS** filesystem, and provides a preemptive multitasking environment with system calls, a minimal C library, user accounts/permissions, USB (xHCI) input and mass storage, basic networking, and a user shell. It serves as a learning platform for OS development and a foundation for further experimentation.

### Key Concepts

- **Monolithic Kernel** – all core services (memory management, process scheduling, drivers) run in kernel space.
- **UEFI Boot** – boots on modern hardware using the UEFI firmware.
- **Graphical Console** – uses the framebuffer for text output with a custom 8×8 font, 256-color palette, and a scaled/tilted big-text renderer used for the boot logo.
- **Preemptive Multitasking** – the timer (100 Hz) preempts ring-3 code on a fixed timeslice, so a process that never yields can no longer stall the system; process states are READY, RUNNING, BLOCKED, SLEEPING, STOPPED, TERMINATED, with `fork()`/`exec()`/`wait()` and POSIX-style signals.
- **Virtual Memory** – `mmap()`/`munmap()` (anonymous, eager-allocated) back the minimal libc's `malloc()`.
- **ELF Executable Support** – loads and runs 64-bit ELF programs with real `argv`/`envp`.
- **System Calls** – 27 syscalls with user-pointer validation and errno-style error codes.
- **Users & Permissions** – Unix-like `uid`/`gid`/9-bit permission model with persistent accounts (`/etc/passwd`, `/etc/group`).
- **Developer Mode** – a persistent on-disk flag that switches between a quiet boot (with a logo) and a fully verbose diagnostic log; see [`04_logging.md`](documentation/04_logging.md).

---

## Features

### Bootloader

- UEFI application with three boot modes:
  - **Normal** – animated splash screen, fast boot.
  - **Debug** – detailed system information, memory map dumps, table listings.
  - **Safe** – minimal mode for troubleshooting.
- Gathers system information (memory map, framebuffer, ACPI/SMBIOS).
- Loads the kernel and the raw disk image (FAT12 ESP + LufiraFS region — see `documentation/08_filesystem.md`).

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
  - Process creation, scheduling, and termination, with real teardown (page tables, ring-0 stacks, and open file descriptors are freed on exit; orphaned background processes are reaped automatically).
  - **Preemptive** multitasking: the timer (100 Hz) preempts ring-3 code on a fixed timeslice; every process is guaranteed to actually reach ring 3, even one that never calls a syscall.
  - `fork()`, in-place `exec()` with real `argv`/`envp`, `wait()`, and POSIX-style signals (`SIGTERM`, `SIGKILL`, `SIGSTOP`, `SIGCONT`) via `kill`.
  - Anonymous pipes for inter-process communication.
  - `syscall` instruction for fast system calls.

- **Virtual Memory**
  - `mmap()`/`munmap()` — anonymous, eager-allocated, per-process region tracking, `PROT_READ`/`PROT_WRITE`/`PROT_EXEC` enforced via the NX bit.

- **System Calls (27 implemented)**
  - File operations: `open`, `close`, `read`, `write`, `seek`, `pipe`, `mkdir`, `rmdir`, `unlink`, `readdir`.
  - Process control: `exit`, `getpid`, `sleep`, `kill`, `yield`, `fork`, `exec`, `wait`.
  - Memory: `mmap`, `munmap`.
  - Filesystem/identity: `getcwd`, `chdir`, `chmod`, `chown`, `getuid`, `getgid`.
  - System info: `gettick`.
  - Every syscall that dereferences a user pointer validates it first; failures return errno-style codes (`EFAULT`, `EINVAL`, `ENOENT`, `ENOTDIR`, `ERANGE`, `EPERM`, `EACCES`, …) instead of a bare `-1`.

- **Minimal Userspace libc** (`libc/`)
  - `crt0.S` startup (`int main(int argc, char **argv, char **envp)`), thin syscall wrappers.
  - Arena-based `malloc`/`free` built on `mmap`.
  - A `string.h` subset and a small `printf` (`%d %u %x %s %c %p %l*`).

- **Users, Groups & Permissions**
  - Unix-like `uid`/`gid`/9-bit permission model enforced on every file/directory operation; root (`uid` 0) bypasses all checks.
  - Persistent `/etc/passwd` and `/etc/group` (FNV-1a password hashing); `whoami`, `chmod`, `chown`, `useradd`, `groupadd`, `su`.
  - Boots straight into a root shell — no login prompt.

- **Filesystem — LufiraFS**
  - Custom filesystem (superblock, block bitmap, fixed inode table, real `.`/`..` directory entries, per-inode owner/group/permission bits) that replaces FAT as the primary storage backend.
  - A small FAT12 partition (the UEFI ESP) still holds only the bootloader and kernel binary, since UEFI firmware can only read FAT — everything else lives on LufiraFS.
  - Virtual Filesystem (VFS) abstraction layer; shell filesystem commands and syscalls now go through the same `vfs_*_at()` call layer instead of duplicating LufiraFS logic.
  - Dirty block tracking and flushing back to disk.
  - Full path resolution (multi-level directories, per-process cwd) and directory operations (`mkdir`, `rm`, `opendir`, `readdir`).
  - Host-side `mkfs_lufirafs` tool for formatting/populating the disk image at build time.
  - **FAT driver** (`kernel/fs/fat/`) — used to `mount`/`unmount` a USB flash drive's FAT12/16/32 filesystem (read/write); not integrated into the VFS, the whole device image is read into RAM (capped at 8 MB).

- **Drivers**
  - **Console** – graphical text output, 256-color palette, scrollback, scaled/tilted big-text rendering (boot logo, shell watermark).
  - **Disk (ATA PIO)** – sector read/write for primary IDE channel.
  - **Keyboard (PS/2)** – scancode translation, modifiers, IRQ1.
  - **Mouse (PS/2)** – packet decoding, IRQ12.
  - **USB (xHCI)** – full command/event/transfer-ring host-controller driver (replaces the former UHCI driver), with a HID boot-protocol keyboard/mouse driver and USB Mass Storage (Bulk-Only Transport) support, unified with PS/2 through a common input dispatcher.
  - **Network (RTL8139)** – Ethernet driver plus a basic Ethernet/ARP/IPv4/ICMP/TCP stack (polled, no interrupts).
  - **PCI** – bus enumeration, BAR management.
  - **AC’97 Audio** – mixer control, DMA playback, tone generation.

- **ACPI**
  - RSDP parsing (revision 1 and 2).
  - FADT detection and ACPI mode enabling.
  - System shutdown (S5 state).

- **Developer Mode & Logging**
  - Persistent on-disk flag (`/system/devmode.flag`) toggled with the `devmode` command, gating verbose boot/driver diagnostics.
  - Lightweight `klog` logger writes short status lines to `/logs/system.log` regardless of developer mode.

- **Shell** (53 built-in commands)
  - Command-line interface with line editing, command history (20 entries), and tab completion.
  - File management (`ls -l`, `cp`, `mv`, `mkdir`, `rm`, `df`/`du`, …), process control (`run`, `runbg`, `exec`, `kill`, `wait`, `ps`), users/permissions (`whoami`, `chmod`, `chown`, `useradd`, `groupadd`, `su`), USB mass storage (`usbinfo`, `usbread`/`usbwrite`, `mount`/`unmount`), networking (`ifconfig`, `ping`, `wget`), audio, and developer mode.
  - Current working directory (cwd) is per-process, not shell-global.

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
\| | System Calls (27) | |
\| +------------------------------------------+ |
\| | VFS / LufiraFS Driver / Users&Perms | |
\| +------------------------------------------+ |
\| | Process Scheduler / ELF Loader | |
\| | (preemptive, fork / exec / wait / pipes) | |
\| +------------------------------------------+ |
\| | Memory Management (PMM / Paging / Heap / | |
\| | mmap) | |
\| +------------------------------------------+ |
\| | Drivers (Console, Disk, Keyboard, Mouse, | |
\| | USB/xHCI+MSD, RTL8139/Net, PCI, AC'97, | |
\| | ACPI) | |
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
| [`04_logging.md`](https://documentation/04_logging.md)                     | Logging macros (log.h), developer mode, and `klog`          |
| [`05_build_system.md`](https://documentation/05_build_system.md)           | Build system and QEMU usage                                 |
| [`06_libraries.md`](https://documentation/06_libraries.md)                 | System libraries (types, colors, string, etc.)              |
| [`07_drivers.md`](https://documentation/07_drivers.md)                     | Device drivers (console, disk, keyboard, mouse, USB/xHCI + Mass Storage, RTL8139, PCI, AC'97) |
| [`08_filesystem.md`](https://documentation/08_filesystem.md)               | LufiraFS driver, Virtual Filesystem (VFS), FAT/USB mounting  |
| [`09_acpi.md`](https://documentation/09_acpi.md)                           | ACPI subsystem (RSDP, FADT, shutdown)                       |
| [`10_cpu_interrupts.md`](https://documentation/10_cpu_interrupts.md)       | CPU, GDT, IDT, IRQ, TSS, PIT, preemptive scheduling          |
| [`11_memory_management.md`](https://documentation/11_memory_management.md) | PMM, Paging, Heap, `mmap`/`munmap`                           |
| [`12_elf_processes.md`](https://documentation/12_elf_processes.md)         | ELF loader and process management                           |
| [`13_syscalls.md`](https://documentation/13_syscalls.md)                   | System calls (syscall instruction, table, handler)          |
| [`14_shell_commands.md`](https://documentation/14_shell_commands.md)       | Shell and built-in commands                                 |
| [`15_users_permissions.md`](https://documentation/15_users_permissions.md) | Users, groups, and file permissions                          |
| [`16_networking.md`](https://documentation/16_networking.md)               | Network stack (Ethernet/ARP/IPv4/ICMP/TCP)                   |

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
│   │   ├── usb/               # xHCI host controller + USB HID + Mass Storage driver
│   │   ├── net/                # RTL8139 Ethernet driver
│   │   ├── input/             # Shared PS/2 + USB HID input dispatcher
│   │   ├── pci/               # PCI bus driver
│   │   └── sound/             # AC'97 audio driver
│   ├── fs/                    # Filesystem
│   │   ├── lufirafs/          # LufiraFS driver (primary filesystem)
│   │   │   ├── lufirafs.c            # Core implementation
│   │   │   ├── lufirafs_vfs.c        # VFS wrapper
│   │   │   ├── lufirafs.h            # Driver API
│   │   │   └── lufirafs_format.h     # On-disk format (shared with mkfs_lufirafs)
│   │   ├── fat/                # FAT driver, used by mount/unmount for USB drives
│   │   └── vfs/                # Virtual Filesystem
│   │       ├── vfs.c          # VFS core
│   │       └── vfs.h          # VFS header
│   ├── net/                    # Ethernet/ARP/IPv4/ICMP/TCP protocol stack
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
│   │       ├── users.c        # Users/groups/permissions commands
│   │       ├── usb.c          # USB Mass Storage block commands
│   │       ├── mount.c        # FAT mount/unmount commands
│   │       ├── net.c          # Network commands (ifconfig/ping/wget)
│   │       └── sound.c        # Audio commands
│   └── system/                # Kernel subsystems
│       ├── acpi/              # ACPI
│       ├── cpu/               # CPU management (GDT, IDT, IRQ, TSS)
│       ├── devmode/           # Developer-mode flag (persistent, gates DLOG)
│       ├── klog/              # Persistent event logging (/logs/system.log)
│       ├── elf/               # ELF loader
│       ├── mm/                # Memory management (PMM, Paging, Heap, mmap)
│       ├── process/           # Process management (preemptive, fork/exec/signals)
│       ├── syscall/           # System calls
│       ├── users/             # User/group database (/etc/passwd, /etc/group)
│       └── timer/             # PIT timer, preemption timeslice
│
├── libc/                      # Minimal userspace C library
│   ├── crt0.S                 # Startup code (_start -> main(argc, argv, envp))
│   ├── include/                # lufira/syscall.h, string.h, stdlib.h, stdio.h
│   └── src/                   # malloc.c, printf.c, string.c
│
├── tools/                     # Host-side build tools
│   ├── mkfs_lufirafs.c        # LufiraFS formatting/populating tool
│   └── seed/                  # Seeded /etc/passwd, /etc/group content
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
- **`mount` reads the whole USB device into RAM up front** (capped at 8 MB) — no lazy/streaming FAT access, and files on a mounted USB drive aren't reachable through `cat`/`cp`/`ls` (only through `mount`'s own directory listing) — see [`08_filesystem.md`](documentation/08_filesystem.md).
- **No memory protection beyond paging permissions and mmap's own bookkeeping.** `mmap` is anonymous-only, eagerly allocated, and never reclaims address space after `munmap`.
- **No DNS, DHCP, or UDP.** `wget`/`ifconfig` work with literal IP addresses and a static configuration only; the TCP client is a minimal, single-connection, best-effort implementation with no retransmission or congestion control — see [`16_networking.md`](documentation/16_networking.md).
- **No setuid/setgid, no multi-group membership, and a non-cryptographic password hash.** The users/permissions model is a straightforward `uid`/`gid`/9-bit implementation, not a hardened one — see [`15_users_permissions.md`](documentation/15_users_permissions.md).
- **No package manager yet.** Every driver and shell command still ships built into the kernel binary — the explicit scope of the next release (v0.7).
- **Real hardware is untested.** The system is developed and tested exclusively in QEMU; UEFI/ACPI/USB quirks on real firmware are unknown.
- See [CHANGELOG.md](CHANGELOG.md) for issues fixed in past versions.

---

## Contributing

Contributions are welcome! Here are some areas for improvement:

- **Filesystem**: Long file name (LFN) support, additional filesystem drivers (ext2, ISO9660), LufiraFS indirect-block-chain growth beyond a single indirect block, real VFS multi-mount support (so a mounted USB drive is reachable through ordinary filesystem commands).
- **Drivers**: AHCI/SATA, graphics acceleration, multi-block USB Mass Storage transfers.
- **Networking**: DNS, DHCP, UDP, TCP retransmission/congestion control.
- **Processes**: Copy-on-write address spaces, demand-paged `mmap`.
- **Security**: setuid/setgid, cryptographically sound password hashing, VFS-level permission enforcement (currently caller-enforced, not VFS-enforced).
- **Shell**: Redirection, environment variables, scripting.
- **Package Manager**: `.lpg` format, `dlpg`, moving Base commands out of the kernel binary — the scope of v0.7.
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