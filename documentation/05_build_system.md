# Build System

This document describes how LufiraOS is built, assembled into a disk image, and run. Since v0.7, this is **two separate tools in two separate repositories**:

- **This repository's `Makefile`** compiles only the UEFI bootloader and the kernel — `BOOTX64.EFI` and `kernel.bin`. It knows nothing about disk images, QEMU, or userspace packages.
- **The sibling [`LufiraOS-Builder`](https://github.com/VLPLAY-Games/LufiraOS-Builder) repository** (pure tooling, no OS code of its own) takes those two files, stages seed files and userspace `.lpg` packages from the sibling [`lufira-packages`](https://github.com/VLPLAY-Games/lufira-packages) repository onto a LufiraFS region, assembles the bootable `disk.img`, and launches QEMU.

Before v0.7 this was all one Makefile (`make run`/`make debug`/`make monitor`, a disk-image recipe, QEMU invocations, and all). That Makefile has been **cut down to just the kernel/bootloader build** — anything else described by an older version of this document no longer applies here, only in `LufiraOS-Builder`.

---

## Table of Contents

1. [Overview](#overview)
2. [This Repository: `LufiraOS/Makefile`](#this-repository-lufiraosmakefile)
   - [Prerequisites](#prerequisites)
   - [Build Commands](#build-commands)
   - [Build Process](#build-process)
3. [The Sibling Repository: `LufiraOS-Builder`](#the-sibling-repository-lufiraos-builder)
   - [What It Does](#what-it-does)
   - [Where Packages Come From](#where-packages-come-from)
   - [Disk Image Assembly](#disk-image-assembly)
   - [Running in QEMU](#running-in-qemu)
4. [Adding Files to the Disk Image](#adding-files-to-the-disk-image)
5. [Directory Structure](#directory-structure)
6. [Troubleshooting](#troubleshooting)
7. [Dependencies](#dependencies)
8. [Conclusion](#conclusion)

---

## Overview

**Key facts:**
- `LufiraOS/Makefile` produces exactly two artifacts: `build/BOOTX64.EFI` (UEFI bootloader) and `build/kernel.bin` (raw kernel binary). It has no `run`/`debug`/`monitor`/`disk` targets anymore.
- `LufiraOS-Builder/build.py` is a standalone Python CLI (with an optional Tkinter GUI, `gui.py`) that drives everything downstream of those two files: compiling the host-side `mkfs_lufirafs` tool, fetching or building userspace `.lpg` packages, assembling `disk.img`, and launching QEMU.
- `LufiraOS-Builder` expects sibling checkouts (`../LufiraOS`, `../lufira-packages` relative to itself) by default, both overridable with `--lufira-repo`/`--lufira-packages-repo`, and will `git clone` them automatically if missing (`tools.ensure_repo()`).
- By default, `LufiraOS-Builder` does **not** need a local `lufira-packages` checkout or toolchain at all: it downloads prebuilt `.lpg` packages (plus `shell.elf`/`libc.so`) straight from `lufira-packages`' own published `index.json`/`release/` on GitHub, the same place `dlpg sync`/`dlpg upgrade` pull from at runtime (see [`17_package_manager.md`](17_package_manager.md)). Pass `--build-packages-from-source` to build a local `lufira-packages` checkout instead, for package development.

---

## This Repository: `LufiraOS/Makefile`

### Prerequisites

| Tool | Purpose | Package (Ubuntu/Debian) |
|------|---------|-------------------------|
| `gcc` | C compiler | `gcc` |
| `ld` | Linker | `binutils` |
| `objcopy` | Binary conversion | `binutils` |
| `nm` | Symbol listing | `binutils` |
| `make` | Build automation | `make` |
| `truncate` | File size manipulation | `coreutils` |
| GNU-EFI headers/libs | UEFI bootloader | `gnu-efi` (or distro equivalent) |

### Environment Setup

The Makefile assumes:
- GNU-EFI headers: `/usr/include/efi`
- GNU-EFI libraries: `/usr/lib`

### Build Commands

| Command | Description |
|---------|-------------|
| `make all` (default) | Builds `BOOTX64.EFI` and `kernel.bin`. |
| `make bootloader` | Builds only `BOOTX64.EFI`. |
| `make kernel` | Builds only `kernel.bin` (and `kernel.elf`, with debug symbols). |
| `make clean` | Removes `build/`. |
| `make info` | Prints the configured source-file lists (bootloader/kernel C/kernel ASM). |
| `make quick` | `clean` then `all`. |

There is no `make run`, `make debug`, `make monitor`, `make disk`, or `make check-disk` — those all moved to `LufiraOS-Builder` (see below). `LufiraOS-Builder`'s own `--no-build-kernel` flag, when set, skips invoking this Makefile at all and uses whatever `build/BOOTX64.EFI`/`build/kernel.bin` are already on disk.

### Build Process

**Bootloader compilation** (unchanged from earlier versions): each `boot/*.c` file is compiled with GNU-EFI flags (`-fpic -ffreestanding -fno-stack-protector -fshort-wchar -mno-red-zone -std=gnu11`), linked against `-lefi -lgnuefi` with the EFI linker script, then `objcopy`'d to `efi-app-x86_64` format.

**Kernel compilation:** every file in `KERNEL_C_SOURCES`/`KERNEL_ASM_SOURCES` (`Makefile`, kept in sync by hand with the actual `kernel/` tree — this now includes `kernel/net/{udp,dns,tls,http_client}.c` and `kernel/net/crypto/*.c`, `kernel/system/elf/dynlink.c`, and `kernel/system/ipc/mailbox.c`, none of which existed in earlier releases) is compiled with `-m64 -ffreestanding -fno-stack-protector -fno-stack-check -fno-asynchronous-unwind-tables -fno-builtin -mno-red-zone -mgeneral-regs-only -std=gnu11`, linked statically (`-static -nostdlib -z max-page-size=0x1000 -z separate-code --gc-sections`) against the custom `kernel/linker.ld` script, then `objcopy`'d to a raw binary and truncated to the size computed from the linker-defined `__kernel_end` symbol.

`kernel/shell/shell.c` and `kernel/shell/commands/*.c` are still listed in `KERNEL_C_SOURCES` and still compiled into `kernel.bin` — they are dead code (unreachable since the shell moved to userspace, see [`14_shell_commands.md`](14_shell_commands.md)), not something the build system special-cases.

---

## The Sibling Repository: `LufiraOS-Builder`

### What It Does

```bash
# From a sibling checkout: ../LufiraOS and ../lufira-packages (overridable).
python3 build.py run
```

| Subcommand | Description |
|------------|-------------|
| `build` | Assembles `disk.img` without launching anything. |
| `run` | `build`, then launches QEMU with serial on stdio. |
| `debug` | `build` plus the `/tests` payload and an attached USB stick image, then launches QEMU with debug logging. |
| `monitor` | `build`, then launches QEMU with the HMP monitor exposed on `telnet:127.0.0.1:4444`. |
| `clear` | Removes all build output — this repository's own `make clean` plus `LufiraOS-Builder`'s `--out-dir`. |

Useful flags (full list: `python3 build.py <subcommand> --help`):

| Flag | Effect |
|------|--------|
| `--lufira-repo PATH` | Path to the `LufiraOS` checkout (default: sibling `../LufiraOS`). |
| `--lufira-packages-repo PATH` | Path to the `lufira-packages` checkout (default: sibling `../lufira-packages`; only used with `--build-packages-from-source`). |
| `--out-dir PATH` | Where `disk.img` and intermediate files go (default: `build/`). |
| `--no-build-kernel` | Skip invoking `LufiraOS`'s `Makefile`; use whatever `build/BOOTX64.EFI`/`build/kernel.bin` are already there. |
| `--package PATH.lpg` | Install one extra `.lpg` during image assembly (repeatable). |
| `--no-default-packages` | Don't install the default package set (`shell.elf`/`libc.so` are still staged — not optional, the kernel loads `shell.elf` directly on every boot). |
| `--only-package NAME` | Install only the named default package(s), instead of all of them. |
| `--build-packages-from-source` | Build `lufira-packages` locally instead of downloading its prebuilt release. |

A thin Tkinter GUI, `gui.py`, sits on top of the same CLI — every button runs the corresponding `build.py` subcommand as a subprocess and streams its output, so it can never drift from the CLI's own behavior.

### Where Packages Come From

By default (`fetch_default_packages_remote()`, `lufira_builder/packages.py`), `LufiraOS-Builder` downloads `index.json` and every default package's `.lpg` from `lufira-packages`' own GitHub repository (`raw.githubusercontent.com` — the same index `dlpg sync` fetches at runtime, see [`17_package_manager.md`](17_package_manager.md)), plus `shell.elf`/`libc.so` directly (these two are staged files, not `.lpg` packages — see below). Downloads are cached by SHA-256 so repeat builds only re-fetch what changed, and a checksum mismatch against what `index.json` claims is a hard build error rather than a silently-corrupt image.

Passing `--build-packages-from-source` instead builds a local `lufira-packages` checkout via its own `build.py` (see that repository's README) — needed for actually developing packages, not for a normal build.

### Disk Image Assembly

`lufira_builder/image.py` (this logic used to be the `Makefile`'s disk-image recipe) builds `disk.img` in the same two stages as before the split:

**Stage 1 — the ESP (FAT12):** a `LUFIRAFS_ESP_SIZE`-sized (4 MiB) FAT12 image holding `/EFI/BOOT/BOOTX64.EFI` and `/kernel.bin`, written via `mkfs.fat`/`mmd`/`mcopy`, then `dd`'d into `disk.img` at offset 0. `ESP_SIZE` (`lufira_builder/config.py`) must stay in sync with `LUFIRAFS_ESP_SIZE` in this repository's `kernel/fs/lufirafs/lufirafs_format.h` — the same constraint the old Makefile recipe had, just checked from the other repository now.

**Stage 2 — the LufiraFS region:** built with the host-compiled `mkfs_lufirafs` (`tools/mkfs_lufirafs.c`, compiled fresh by `LufiraOS-Builder` itself — see [`08_filesystem.md`](08_filesystem.md#the-mkfs_lufirafs-tool)):
1. `format` — fresh superblock/bitmap/inode table.
2. `mkdir` — `/system`, `/logs`, `/etc`, `/bin`, `/lib`.
3. `put` — `/readme.txt`, `/etc/passwd`/`/etc/group` (from this repository's `tools/seed/`), `/bin/shell.elf` and `/lib/libc.so` (from `lufira-packages`, staged directly — **not** `.lpg` packages: the kernel loads `shell.elf` by hardcoded path on every boot/shell-respawn, and `dlpg` has no mechanism to "remove" either one, so they can't go through the normal package-install path).
4. Every default (or `--package`/`--only-package`-selected) `.lpg` is then unpacked straight onto the image using the **same install logic as `dlpg install`** (`lufira_builder/lpg.py`'s `plan_install()`, shared code with `dlpg` itself) — dependency checking, file extraction, and `/etc/packages/installed`/`/etc/packages/<name>.files` receipts — just writing through `mkfs_lufirafs put` instead of through a running kernel's syscalls. The practical effect: a freshly built image already has every default package "installed" from the very first boot, exactly as if someone had run `dlpg install` on each one by hand; `dlpg` itself only matters for packages added *after* boot.

### Running in QEMU

`lufira_builder/qemu.py` replaces the old Makefile's `run`/`debug`/`monitor` targets. All three share a common prefix:

```
qemu-system-x86_64 -machine pc,accel=kvm:tcg -bios <OVMF path> -drive file=disk.img,format=raw,if=ide,index=0
```

`accel=kvm:tcg` opportunistically uses hardware acceleration (`/dev/kvm`) when available and falls back to software emulation otherwise — this matters more than it used to, because `dlpg sync`/`upgrade`'s TLS handshake (RSA modular exponentiation, see [`16_networking.md`](16_networking.md)) is noticeably slower under pure TCG and can otherwise look like a hang.

| Mode | Extra flags |
|------|-------------|
| `run` | 128 MB RAM, `-netdev user,id=net0 -device rtl8139,netdev=net0` (real network, unlike the old `-net none`), AC'97 audio, xHCI + USB keyboard/mouse, serial on stdio. |
| `debug` | 256 MB RAM, same networking/USB, plus `-no-reboot -no-shutdown`, `-d int,cpu_reset,guest_errors` logged to `build/qemu_debug.log`, a `/system/devmode.flag` dropped onto the image first (enabling [developer mode](04_logging.md) for that run), the `lufira-tests` `.elf` payload staged under `/tests`, and an attached FAT12 USB stick image with a test file on it. |
| `monitor` | Same as `run`, plus `-monitor telnet:127.0.0.1:4444,server,nowait` and an attached (blank) USB stick image. |

Networking is no longer disabled by default — the whole point of `run`/`debug`/`monitor` using QEMU's user-mode networking (SLIRP) is so the real network stack (DNS/TCP/TLS/HTTP, see [`16_networking.md`](16_networking.md)) and `dlpg sync`/`upgrade` have something to talk to.

---

## Adding Files to the Disk Image

- **A new default package:** add it to `lufira-packages` (own build/release process — see that repository's README) and it will show up in `index.json`, picked up automatically by `fetch_default_packages_remote()`.
- **A one-off extra package for a single build:** `python3 build.py run --package /path/to/thing.lpg`.
- **A raw file outside the package system** (like the seed `/etc/passwd`/`/etc/group` or `/readme.txt`): add a `mkfs(mkfs_bin, "put", ...)` call in `lufira_builder/image.py`'s `populate_lufirafs()`, following the existing pattern.

There is no host-side way to list a directory's contents inside `disk.img` — boot the image and use the shell's `ls`/`cat` (see [`14_shell_commands.md`](14_shell_commands.md)).

---

## Directory Structure

```
LufiraOS/                      # this repository — kernel + bootloader ONLY
├── boot/                      # Bootloader sources
├── kernel/                    # Kernel sources (incl. net/, system/elf/dynlink.c, system/ipc/)
├── tools/                     # Host-side build tools (mkfs_lufirafs, lpg_pack, lpg_format.h) + seed/
├── libc/                      # Userspace libc sources (built by lufira-packages' build.py, not this Makefile)
├── build/                     # BOOTX64.EFI, kernel.bin, kernel.elf, object files — NOTHING ELSE
├── Makefile                   # kernel.bin + BOOTX64.EFI only
└── documentation/             # this directory

LufiraOS-Builder/               # sibling repository — disk image + QEMU
├── build.py                   # CLI entry point (build/run/debug/monitor/clear)
├── gui.py                     # optional Tkinter front-end
└── lufira_builder/
    ├── config.py               # sizes, defaults, repo URLs
    ├── image.py                 # disk.img assembly
    ├── packages.py               # .lpg fetch/build selection
    ├── lpg.py                     # .lpg format helpers (shared install logic with dlpg)
    ├── qemu.py                     # QEMU invocation
    └── tools.py                     # host-tool wrappers, sibling-repo auto-clone

lufira-packages/                # sibling repository — userspace: shell, packages, GUI/WM, dlpg
```

---

## Troubleshooting

### Missing Tools (this repository)

**Error:** `Required tool 'xxx' not found in PATH`

**Solution:** install `gcc binutils make` plus GNU-EFI headers/libraries for your distribution.

### Missing Tools / OVMF (`LufiraOS-Builder`)

`LufiraOS-Builder` needs `qemu-system-x86_64`, `mkfs.fat`, `mmd`/`mcopy` (`mtools`), `dd`, and OVMF firmware, same as the old Makefile did. If OVMF isn't at `/usr/share/ovmf/OVMF.fd`, adjust `BIOS_PATH` in `lufira_builder/config.py`.

### Kernel Size Calculation (this repository)

**Error:** `__kernel_end` symbol not found.

**Solution:** ensure `kernel/linker.ld` defines `__kernel_end = .;` and that `nm build/kernel.elf` can find it.

### `disk.img` / QEMU issues

Anything related to disk-image assembly, package staging, or QEMU flags is now in `LufiraOS-Builder`, not here — check that repository's own output and `--help` text first.

---

## Conclusion

The build is now two small, single-purpose tools instead of one large Makefile: `LufiraOS/Makefile` only ever has to know how to turn this repository's own sources into `kernel.bin`/`BOOTX64.EFI`, and `LufiraOS-Builder` owns everything that depends on *other* repositories (`lufira-packages`' packages) or external tools (QEMU, `mkfs.fat`, `mtools`) to assemble and run a bootable image. This mirrors the same split that moved the shell and package manager out of the kernel binary itself (see [`14_shell_commands.md`](14_shell_commands.md), [`17_package_manager.md`](17_package_manager.md)): the kernel repository stays small and focused, and everything downstream of it lives where it actually belongs.

For more details, refer to the source in this repository's `Makefile`, and in `LufiraOS-Builder`'s `build.py`/`lufira_builder/`.

---

**Document Version:** 2.0
**Last Updated:** October 2026
**Project:** LufiraOS
