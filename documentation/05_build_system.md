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
   - [Driver Selection](#driver-selection)
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
- `LufiraOS-Builder` expects sibling checkouts (`../LufiraOS`, `../lufira-packages` relative to itself) by default, both overridable with `--lufira-repo`/`--lufira-packages-repo`, and will `git clone` them automatically if missing (`tools.ensure_repo()`). A **freshly cloned** repository is then pinned to its latest SemVer release tag (`v*`, highest first) via `tools._checkout_latest_release_tag()`, instead of being left on the default branch's HEAD — so a from-scratch `python3 build.py run` builds the latest *released* `LufiraOS`, not whatever is mid-development on `main`. No-op for `lufira-packages` (or any repo with no `v*` tags at all), which ships releases as prebuilt `.lpg` via its own `index.json` instead — see below. An *existing* checkout at that path is left alone either way; this only applies the moment a clone happens.
- `LufiraOS-Builder` supports three disk controllers, two audio controllers, and three NICs for `run`/`debug`/`monitor` — `--disk-mode ide|ahci|nvme`, `--audio ac97|hda|none`, `--net-device rtl8139|e1000|e1000e|none` (`lufira_builder/config.py`'s `DISK_CHOICES`/`AUDIO_CHOICES`/`NET_CHOICES`, expanded into QEMU device strings by `lufira_builder/qemu.py`) — picking which of LufiraOS's own driver code paths (`kernel/drivers/disk`, `kernel/drivers/sound`, `kernel/drivers/net`) actually gets exercised. The GUI (`gui.py`) exposes the same three choices as dropdowns in an "Emulated devices" panel, next to a separate "Legacy BIOS" vs "UEFI" launch-mode picker (see [`01_bootloader.md`](01_bootloader.md#legacy-bios-boot-boot_legacy)) that only becomes selectable once `--legacy-bios` is checked.
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

**Kernel compilation:** every file in `KERNEL_C_SOURCES`/`KERNEL_ASM_SOURCES` (`Makefile`, kept in sync by hand with the actual `kernel/` tree — this includes `kernel/system/elf/dynlink.c` and `kernel/system/ipc/mailbox.c`, and, as of v0.9.5, `kernel/system/driver/driver.c` for the userspace-driver claim model; it no longer includes `kernel/net/` or `kernel/drivers/net/`/`sound/` at all, since v0.9.5 moved all of that into the `netd`/`soundd` userspace daemons — see [`07_drivers.md`](07_drivers.md#userspace-driver-infrastructure)) is compiled with `-m64 -ffreestanding -fno-stack-protector -fno-stack-check -fno-asynchronous-unwind-tables -fno-builtin -mno-red-zone -mgeneral-regs-only -std=gnu11`, linked statically (`-static -nostdlib -z max-page-size=0x1000 -z separate-code --gc-sections`) against the custom `kernel/linker.ld` script, then `objcopy`'d to a raw binary and truncated to the size computed from the linker-defined `__kernel_end` symbol.

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
| `--driver NAME` | *(v0.9.5)* Install only the named userspace driver daemon(s) — `netd` and/or `soundd` (`lufira_builder/config.py`'s `DRIVER_PACKAGES`) — instead of both. Independent of `--only-package`/`--no-default-packages`: the two selections are applied separately, so excluding a daemon from one doesn't require repeating it in the other. See [Driver Selection](#driver-selection) below. |
| `--no-drivers` | *(v0.9.5)* Don't install either driver daemon — an image with no networking or sound at all, e.g. for a minimal/offline build. |
| `--build-packages-from-source` | Build `lufira-packages` locally instead of downloading its prebuilt release. |
| `--legacy-bios` | Also embed the legacy BIOS boot chain (`boot_legacy/` in `LufiraOS` — `stage1.bin`/`stage2.bin`/`legacy_main.bin`, built via `make bootloader-bios`, which needs `nasm`) into `disk.img` alongside the UEFI path — one image, bootable both ways. See [`01_bootloader.md`](01_bootloader.md#legacy-bios-boot-boot_legacy). |
| `--boot-mode uefi\|bios` | Which firmware `run`/`debug`/`monitor` launch QEMU with (default `uefi`); `bios` only works on an image built with `--legacy-bios` (QEMU then falls back to its built-in SeaBIOS instead of `-bios <OVMF>`). |
| `--audio ac97\|hda\|none` | Which audio controller `run`/`debug`/`monitor` attach (default `ac97`) — exercises the AC'97 or HDA code path inside the `soundd` userspace daemon (`lufira-packages/daemons/soundd.c`, moved out of `kernel/drivers/sound/` in v0.9.5 — see [`07_drivers.md`](07_drivers.md#audio-drivers-ac97--intel-hda)) in the guest; `none` omits an audio device entirely. Has no effect if `soundd` itself wasn't built onto the image (see `--driver`/`--no-drivers` above). |
| `--net-device rtl8139\|e1000\|e1000e\|none` | Which NIC `run`/`debug`/`monitor` attach (default `rtl8139`) — exercises the matching driver inside the `netd` userspace daemon (`lufira-packages/daemons/netd/`, moved out of `kernel/drivers/net/` in v0.9.5 — see [`07_drivers.md`](07_drivers.md#network-drivers-rtl8139--e1000--e1000e)); `none` omits a NIC entirely. Has no effect if `netd` itself wasn't built onto the image. |
| `--disk-mode ide\|ahci\|nvme` | Which controller `run`/`debug`/`monitor` attach `disk.img` through (default `ide`, legacy ATA) — `ahci`/`nvme` exercise `kernel/drivers/disk/ahci.c` or `nvme.c` instead. |
| `--bios-path PATH` | Override the auto-detected OVMF firmware path (see Troubleshooting below). |

A thin Tkinter GUI, `gui.py`, sits on top of the same CLI — every button runs the corresponding `build.py` subcommand as a subprocess and streams its output, so it can never drift from the CLI's own behavior. As of v0.9.5, the GUI has a dedicated "Userspace drivers" section (modeled on the existing "Boot firmware"/"Emulated devices" panels) with one checkbox per daemon — `netd`/`soundd` are removed from the general package tree so they aren't offered twice.

### Driver Selection

*(v0.9.5)* `lufira_builder/config.py`'s `DRIVER_PACKAGES = ("netd", "soundd")` is the single source of truth both the CLI and GUI read from. `--driver NAME` (repeatable) and `--no-drivers` work the same way `--only-package`/`--no-default-packages` already did for ordinary packages, but as an **independent** selection — a build can, say, include every default package *and* exclude `soundd`, or vice versa, without the two controls interfering with each other. This exists because `netd`/`soundd` are privileged daemons (see [`07_drivers.md`](07_drivers.md#userspace-driver-infrastructure)), not ordinary coreutils-style packages, so picking them deserves its own clearly-labeled control rather than being buried in the general package list.

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
| `run` | 128 MB RAM, networking via `--net-device` (default `-netdev user,id=net0 -device rtl8139,netdev=net0` — real network, unlike the old `-net none`), audio via `--audio` (default AC'97), disk attached via `--disk-mode` (default legacy IDE), xHCI + USB keyboard/mouse, serial on stdio. |
| `debug` | 256 MB RAM, same networking/audio/disk selection, plus `-no-reboot -no-shutdown`, `-d int,cpu_reset,guest_errors` logged to `build/qemu_debug.log`, a `/system/devmode.flag` dropped onto the image first (enabling [developer mode](04_logging.md) for that run), the `lufira-tests` `.elf` payload staged under `/tests`, and an attached FAT12 USB stick image with a test file on it. |
| `monitor` | Same as `run`, plus `-monitor telnet:127.0.0.1:4444,server,nowait` and an attached (blank) USB stick image. |

Networking is no longer disabled by default — the whole point of `run`/`debug`/`monitor` using QEMU's user-mode networking (SLIRP) is so the real network stack (DNS/TCP/TLS/HTTP, see [`16_networking.md`](16_networking.md)) and `dlpg sync`/`upgrade` have something to talk to.

**Hardware choice flags** (`lufira_builder/qemu.py`): `--disk-mode` picks how `disk.img` is attached — `ahci` adds `-device ahci,id=ahci0` plus an `ide-hd` drive on that bus, `nvme` adds a bare `-device nvme,drive=maindisk,...`, `ide` (default) keeps the historical `-drive file=disk.img,if=ide,index=0`; LufiraOS's own `disk_init()` probes AHCI, then NVMe, then falls back to legacy ATA PIO regardless of which one QEMU presents, so all three boot. `--audio` picks `-device intel-hda` + `hda-duplex` or `-device AC97`, both routed through one shared `driver=alsa` audiodev (the one backend that works headless without a running PulseAudio/PipeWire session). `--net-device` maps directly to QEMU's own device model name (`rtl8139`/`e1000`/`e1000e`) attached to a single `-netdev user,id=net0`.

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
├── boot/                      # Bootloader sources (UEFI path)
├── boot_legacy/                # Legacy BIOS boot chain (stage1.asm/stage2.asm/legacy_main.c) — see 01_bootloader.md
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

`LufiraOS-Builder` needs `qemu-system-x86_64`, `mkfs.fat`, `mmd`/`mcopy` (`mtools`), `dd`, and OVMF firmware, same as the old Makefile did. `lufira_builder/config.py` auto-detects OVMF by checking a list of known per-distro paths, in order, and using the first one that actually exists on disk:

| Path | Distro / package |
|------|-------------------|
| `/usr/share/ovmf/OVMF.fd` | Debian/Ubuntu (`ovmf`) |
| `/usr/share/edk2/x64/OVMF.4m.fd` | Arch Linux (`edk2-ovmf`) |
| `/usr/share/edk2-ovmf/x64/OVMF.fd` | some Arch derivatives |
| `/usr/share/OVMF/OVMF_CODE.fd` | Fedora/RHEL/CentOS (`edk2-ovmf`) |
| `/usr/share/edk2/ovmf/OVMF_CODE.fd` | older Fedora layout |
| `/usr/share/qemu/ovmf-x86_64-code.bin` | openSUSE |

If none of these exist, it falls back to the Debian/Ubuntu path so a missing-firmware error still points at a familiar, googleable path. Override the result entirely with `--bios-path PATH` (or edit `BIOS_PATH`/`_OVMF_CANDIDATES` in `lufira_builder/config.py` directly).

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

**Document Version:** 2.1
**Last Updated:** October 2026
**Project:** LufiraOS
