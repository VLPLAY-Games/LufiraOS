# Filesystem Subsystem

This document describes the filesystem subsystem of LufiraOS, which consists of the **LufiraFS** driver (the primary, custom filesystem) and a Virtual Filesystem (VFS) abstraction layer. Together, they provide persistent storage access and a uniform API for file operations.

---

## Table of Contents

1. [Overview](#overview)
2. [Disk Layout](#disk-layout)
3. [Architecture](#architecture)
4. [LufiraFS On-Disk Format](#lufirafs-on-disk-format)
   - [Superblock](#superblock)
   - [Block Bitmap](#block-bitmap)
   - [Inode Table](#inode-table)
   - [Directory Entries](#directory-entries)
5. [LufiraFS Driver](#lufirafs-driver)
   - [Initialisation](#initialisation)
   - [Path Resolution](#path-resolution)
   - [File Operations](#file-operations)
   - [Directory Operations](#directory-operations)
   - [Dirty Tracking and Flushing](#dirty-tracking-and-flushing)
6. [Virtual Filesystem (VFS)](#virtual-filesystem-vfs)
   - [Core Concepts](#core-concepts)
   - [Inodes](#inodes)
   - [File Descriptors](#file-descriptors)
   - [File Operations](#file-operations-1)
   - [Inode Operations](#inode-operations)
   - [Per-Process File Tables](#per-process-file-tables)
7. [LufiraFS VFS Wrapper](#lufirafs-vfs-wrapper)
8. [The `mkfs_lufirafs` Tool](#the-mkfs_lufirafs-tool)
9. [Special Devices](#special-devices)
10. [Legacy FAT Driver](#legacy-fat-driver)
11. [Dependencies](#dependencies)
12. [Future Extensions](#future-extensions)

---

## Overview

The filesystem subsystem provides two main layers:

**LufiraFS Driver (Raw Layer)**
The low-level implementation of LufiraOS's own filesystem. It operates on a disk image loaded into memory, manages data blocks via a bitmap, parses a fixed-size inode table, and supports read and write operations with dirty-block tracking.

**Virtual Filesystem (VFS)**
A generic abstraction layer that presents files, directories, and devices as inodes and file descriptors. It dispatches operations to the underlying filesystem driver and provides a unified API for userspace programs and shell commands.

LufiraFS was written to remove the dependency on FAT (and its limitations — root-directory-only lookups, no real `.`/`..` handling, hardcoded parent clusters in several commands) for anything beyond what UEFI firmware itself requires.

---

## Disk Layout

UEFI firmware can only read FAT12/16/32 — this is a hard requirement of the UEFI specification, not a design choice. Because of this, the disk is split into two regions with no partition table:

| Region | Size | Format | Contents |
|--------|------|--------|----------|
| ESP (Elementary/EFI System Partition) | `LUFIRAFS_ESP_SIZE` (4 MiB) | FAT12 | `/EFI/BOOT/BOOTX64.EFI`, `/kernel.bin` — read directly by UEFI firmware and the bootloader |
| LufiraFS region | remaining space (12 MiB in the default 16 MiB disk image) | LufiraFS | everything else: user files, `/system`, `/logs`, etc. |

The bootloader (`boot/loaders/fat_loader.c`) is unaware of this split — it already loads the **entire raw disk** into RAM starting at LBA 0 (`bi->FATImageBase`/`bi->FATImageSize`), regardless of what filesystem(s) live where. The kernel simply computes:

```c
void   *fs_image   = (void*)(bi->FATImageBase + LUFIRAFS_ESP_SIZE);
uint32_t fs_size   = bi->FATImageSize - LUFIRAFS_ESP_SIZE;
uint32_t lba_offset = LUFIRAFS_ESP_SIZE / 512;
lufirafs_init(&lufirafs, fs_image, fs_size, lba_offset);
```

and mounts LufiraFS over the remaining bytes. `LUFIRAFS_ESP_SIZE` must be kept in sync between the kernel (`lufirafs_format.h`) and the `Makefile`'s image-build recipe — a mismatch means `mkfs_lufirafs` formats a different region than the one the kernel reads.

---

## Architecture

The filesystem subsystem follows a layered architecture:

**Userspace / Shell**
Applications and shell commands use the VFS API (or, for shell commands that need cwd-relative resolution, the LufiraFS API directly) for all file operations.

**VFS Layer**
Provides a uniform interface for file operations. Dispatches calls to the underlying filesystem driver via function pointers. VFS-facing lookups always resolve from the root inode, matching what userland ELF syscalls expect.

**LufiraFS VFS Wrapper**
Converts VFS operations to LufiraFS driver calls. Implements inode and file operations for LufiraFS.

**LufiraFS Driver**
Low-level implementation. Manages the block bitmap, inode table, directory entries, and the disk image in memory.

**Disk Image (Memory)**
The LufiraFS region of the disk, loaded by the bootloader as part of the raw disk image, residing in physical memory.

**ATA Driver**
Only used during flush operations (`lufirafs_sync()`/`lufirafs_flush()`) to write dirty blocks back to disk.

---

## LufiraFS On-Disk Format

The on-disk format is defined once, in `kernel/fs/lufirafs/lufirafs_format.h`, and shared verbatim between the freestanding kernel driver and the hosted `tools/mkfs_lufirafs.c` tool — both include the exact same header, so the two sides can never disagree about layout arithmetic.

The LufiraFS region is laid out as one contiguous sequence of fixed-size (4096-byte) blocks:

```
block 0                                    superblock
[bitmap_start .. +bitmap_blocks)           block bitmap (1 bit per block, including reserved blocks)
[inode_table_start .. +inode_table_blocks) inode table (fixed size)
[data_start .. total_blocks)               data blocks (file/directory contents)
```

`lufirafs_compute_layout()` computes `bitmap_start`/`bitmap_blocks`/`inode_table_start`/`inode_table_blocks`/`data_start` from `total_blocks` — this is the single source of truth used by both the kernel and `mkfs_lufirafs`.

### Superblock

| Field | Description |
|-------|-------------|
| `magic` | `0x31534C4F` ("OLS1" in little-endian bytes) — identifies a formatted LufiraFS region. |
| `version` | Format version (currently 1). |
| `block_size` | Always 4096 bytes. |
| `total_blocks` | Total blocks in the region. |
| `bitmap_start` / `bitmap_blocks` | Location and size of the block bitmap. |
| `inode_table_start` / `inode_table_blocks` | Location and size of the inode table. |
| `inode_count` | Fixed at 512 inodes, independent of region size. |
| `data_start` | First data block. |
| `root_inode` | Inode number of `/` (always 1; inode 0 is reserved as "no inode"). |
| `free_blocks` / `free_inodes` | Cached statistics for `df`/`du` — recomputed if they ever disagree with the bitmap. |

### Block Bitmap

One bit per block for the **entire** region, including the superblock, the bitmap itself, and the inode table — all of these are marked used at format time so the allocator never hands them out.

### Inode Table

Fixed-size table of 512 inodes, 64 bytes each:

| Field | Description |
|-------|-------------|
| `mode` | `LUFIRAFS_MODE_FREE` (0), `LUFIRAFS_MODE_FILE` (1), or `LUFIRAFS_MODE_DIR` (2). |
| `size` | Size in bytes (for directories, bytes of directory-entry data). |
| `links_count` | ≥1 while the inode is live; 0 means free. |
| `direct[12]` | 12 direct block pointers. |
| `indirect` | One single-indirect block pointer (1024 more pointers). |

Maximum file size is `(12 + 1024) * 4096` bytes (~4.2 MiB) — there is no double-indirect block.

### Directory Entries

Directories store fixed 64-byte entries (`uint32_t inode` + `char name[60]`, 59 usable characters). `mkdir` creates **real** `.` and `..` entries at creation time (root's `..` points to itself), which removes the need for any FAT-style special-casing of `.`/`..` in the driver or in shell commands, and enables full multi-level path resolution.

---

## LufiraFS Driver

### Initialisation

`lufirafs_init(fs, image, image_size, lba_offset)`:

1. Stores the `image` pointer and `image_size`.
2. Copies the superblock from the start of the image and validates `magic` and `block_size`.
3. Allocates a dirty-block bitmap (`kmalloc`, `(total_blocks + 7) / 8` bytes).
4. Sets the global `lufirafs_mounted` flag on success — checked by `devmode`/`klog` before they touch the filesystem.

### Path Resolution

`lufirafs_lookup(fs, start_inode, path, &out_inode)` resolves an absolute (`/a/b`) or relative path (starting from `start_inode`, typically the caller's cwd). `.` and `..` are ordinary directory entries, so no special-case logic is needed.

`lufirafs_resolve_parent(fs, start_inode, path, &out_parent, out_name)` splits a path into a parent inode and a final component name — used by `create`/`mkdir`/`unlink`, whose target does not need to exist yet.

### File Operations

| Function | Description |
|----------|-------------|
| `lufirafs_read(fs, ino, offset, buf, count)` | Reads up to `count` bytes starting at `offset`. |
| `lufirafs_write(fs, ino, offset, buf, count)` | Writes `count` bytes at `offset`, growing the file (and allocating blocks) as needed; updates `inode.size`. |
| `lufirafs_truncate(fs, ino, new_size)` | Shrinks or clears a file, freeing now-unused blocks. |
| `lufirafs_create(fs, parent_ino, name, mode, &out_ino)` | Creates a file or directory; for directories, also creates `.`/`..`. |
| `lufirafs_unlink(fs, parent_ino, name)` | Removes a file or empty directory. |

### Directory Operations

`lufirafs_opendir()`/`lufirafs_readdir()` provide a simple cursor-based iterator over a directory's entries, used both by the VFS wrapper and directly by shell commands (`ls`, `du`, etc.).

`lufirafs_du_blocks(fs, ino)` recursively counts the blocks actually occupied by a file or directory tree (including indirect blocks), matching what real Unix `du` reports — as opposed to the logical file size (`inode.size`).

### Dirty Tracking and Flushing

The entire LufiraFS region lives in RAM (`fs->image`); all reads and writes touch this RAM copy directly. A per-block dirty bitmap (`fs->dirty_bitmap`) tracks which 4096-byte blocks have changed.

`lufirafs_sync()`/`lufirafs_flush()` scan the dirty bitmap and write each dirty block back to disk via the existing ATA PIO driver (`disk_write_sectors()`), computing each block's absolute LBA as `fs->lba_offset + block_num * (LUFIRAFS_BLOCK_SIZE / 512)`. This mirrors the persistence strategy of the (now unused) FAT driver. Sync is called after every mutating shell operation and explicitly on `reboot`/`shutdown`.

---

## Virtual Filesystem (VFS)

### Core Concepts

**Inodes**
An inode represents a filesystem object (file, directory, or device). It contains:

- **inode number** – unique identifier within the filesystem.
- **type** – file, directory, character device, block device, pipe, or symlink.
- **size** – size of the object in bytes.
- **reference count** – number of open file descriptors referencing this inode.
- **private data** – filesystem-specific data (LufiraFS inode number and directory cursor).
- **operations** – function pointers for inode operations.

**Files**
A file represents an open file descriptor. It contains:

- **file descriptor number** – unique within the process.
- **inode** – pointer to the underlying inode.
- **offset** – current position within the file.
- **flags** – open mode (read, write, create, truncate, append).
- **operations** – function pointers for file operations.

### Inodes

Inodes are created by `vfs_create_inode()`, which allocates memory and initialises the structure. The VFS inode number is the LufiraFS inode number (or 1 for the root directory).

### File Descriptors

File descriptors are allocated by `alloc_fd()`, which searches the current process's file table for an empty slot. Each process has its own file descriptor table.

### File Operations

| Operation | Description |
|-----------|-------------|
| `read` | Reads data from the file into a buffer. |
| `write` | Writes data from a buffer to the file. |
| `seek` | Moves the file offset. |
| `close` | Closes the file descriptor. |

### Inode Operations

| Operation | Description |
|-----------|-------------|
| `lookup` | Finds a file or directory by name within a directory. |
| `create` | Creates a new file or directory. |
| `remove` | Deletes a file or directory. |
| `readdir` | Reads the next directory entry. |

### Per-Process File Tables

Each process has its own file descriptor table (`fd_table_t`), initialised by `vfs_init_fd_table()` for every new process (not just the first one). The current process's table is pointed to by `current_fd_table`. `fork()` duplicates the parent's table (shared `file_t`/inode with an incremented reference count); anonymous pipes (`vfs_pipe()`) and `dup2()`-style redirection build on this same table.

---

## LufiraFS VFS Wrapper

The LufiraFS VFS wrapper (`lufirafs_vfs.c`) bridges the raw LufiraFS driver and the VFS layer, mirroring the design of the old FAT wrapper.

**Private Data:** `lufirafs_private_t` (stored in `inode_t::private_data`) holds the LufiraFS inode number, whether the object is a directory, and (for directories) a `lufirafs_dir_t` cursor.

**VFS entry points:** `vfs_open_lufirafs`, `vfs_lufirafs_create`, `vfs_lufirafs_mkdir`, `vfs_lufirafs_unlink`, `vfs_lufirafs_lookup`, `vfs_lufirafs_get_root` — these replace the equivalent `vfs_fat_*` functions in `kernel/fs/vfs/vfs.c` one-for-one.

VFS-facing lookups always resolve from the LufiraFS root inode. Shell commands that need cwd-relative behaviour (`cd`, `ls`, `mkdir`, etc.) call the lower-level `lufirafs_*` functions directly with the shell's own `cwd_inode`, rather than going through the VFS layer.

---

## The `mkfs_lufirafs` Tool

`tools/mkfs_lufirafs.c` is a normal hosted C program (built with the host's `gcc`, full libc) that formats and populates the LufiraFS region of the disk image at build time. It `#include`s the same `lufirafs_format.h` used by the freestanding kernel driver, guaranteeing format compatibility, and reimplements a minimal block/inode allocator and directory-entry manager independently (intentionally not shared code, since one side is freestanding and the other hosted).

**Subcommands:**

| Subcommand | Usage | Description |
|------------|-------|-------------|
| `format` | `mkfs_lufirafs format <image> <esp_size> <region_size>` | Writes a fresh superblock, bitmap, and empty inode table. |
| `mkdir` | `mkfs_lufirafs mkdir <image> <esp_size> <region_size> </path>` | Creates a directory, including any missing intermediate directories ("mkdir -p" style). |
| `put` | `mkfs_lufirafs put <image> <esp_size> <region_size> <host_file> </dest/path>` | Copies a host file into the image, creating parent directories as needed. |

The `Makefile`'s disk-image recipe uses these to create `/test`, `/system`, `/logs`, and `/readme.txt` when building `disk.img`, and `make debug` uses `put` to drop a `/system/devmode.flag` marker before launching QEMU (see [`04_logging.md`](04_logging.md)).

---

## Special Devices

The VFS supports a console device at `/dev/console` (or simply `console`). Writing to this device prints to the screen; reading returns 0.

The console device uses the `file_ops_t` structure with:
- `read` – returns 0 (no input).
- `write` – prints characters to the console.
- `seek` – returns -1 (not supported).
- `close` – does nothing.

Standard file descriptors (stdin, stdout, stderr) are connected to the console device. Anonymous pipes (`vfs_pipe()`) use a similar `file_ops_t`-based approach backed by a fixed-size ring buffer.

---

## Legacy FAT Driver

The original FAT12/16/32 driver (`kernel/fs/fat/`) is still present in the source tree but is **no longer compiled into the kernel** — it has been fully superseded by LufiraFS for anything the kernel itself mounts. It is kept only for reference. The UEFI ESP partition (see [Disk Layout](#disk-layout)) is still FAT12, but it is read exclusively by UEFI firmware and the bootloader's own FAT reader, not by this kernel driver.

---

## Dependencies

| Component | Depends On | Purpose |
|-----------|------------|---------|
| LufiraFS Driver | Heap | Memory allocation for the dirty-block bitmap |
| LufiraFS Driver | ATA Driver | Flushing dirty blocks to disk |
| LufiraFS Driver | Console | Error reporting and logging (gated by developer mode — see `04_logging.md`) |
| VFS | Heap | Memory allocation for inodes and files |
| VFS | LufiraFS Driver | Underlying filesystem operations |
| LufiraFS VFS Wrapper | LufiraFS Driver, VFS | Bridge between layers |
| Special Devices | Console | Output operations |

---

## Conclusion

The filesystem subsystem provides a solid foundation for persistent storage and device I/O. The separation between the low-level LufiraFS driver and the generic VFS layer keeps the system extensible and maintainable, while the shared on-disk format header keeps the kernel driver and the host-side `mkfs_lufirafs` tool from ever disagreeing about layout.

For more details, refer to the source code in `fs/lufirafs/`, `fs/vfs/`, and `tools/mkfs_lufirafs.c`.

---

**Document Version:** 2.0
**Last Updated:** September 2026
**Project:** LufiraOS
