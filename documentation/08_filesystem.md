# Filesystem Subsystem

This document describes the filesystem subsystem of LufiraOS, which consists of a FAT12/16/32 driver and a Virtual Filesystem (VFS) abstraction layer. Together, they provide persistent storage access and a uniform API for file operations.

---

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [FAT Driver](#fat-driver)
   - [Initialisation](#initialisation)
   - [Boot Sector Parsing](#boot-sector-parsing)
   - [FAT Types](#fat-types)
   - [Directory Operations](#directory-operations)
   - [File Operations](#file-operations)
   - [Cluster Management](#cluster-management)
   - [Dirty Tracking and Flushing](#dirty-tracking-and-flushing)
4. [Virtual Filesystem (VFS)](#virtual-filesystem-vfs)
   - [Core Concepts](#core-concepts)
   - [Inodes](#inodes)
   - [File Descriptors](#file-descriptors)
   - [File Operations](#file-operations-1)
   - [Inode Operations](#inode-operations)
   - [Per-Process File Tables](#per-process-file-tables)
5. [FAT VFS Wrapper](#fat-vfs-wrapper)
   - [Private Data](#private-data)
   - [Path Resolution](#path-resolution)
   - [Directory Iteration](#directory-iteration)
6. [Special Devices](#special-devices)
7. [Dependencies](#dependencies)
8. [Future Extensions](#future-extensions)

---

## Overview

The filesystem subsystem provides two main layers:

**FAT Driver (Raw Layer)**
The low-level implementation of the FAT12/16/32 filesystem. It operates on a disk image loaded into memory, manages clusters, parses directory entries, and supports read and write operations with dirty-block tracking.

**Virtual Filesystem (VFS)**
A generic abstraction layer that presents files, directories, and devices as inodes and file descriptors. It dispatches operations to the underlying filesystem driver and provides a unified API for userspace programs.

This design allows the kernel to support multiple filesystem types in the future without changing the userspace API.

---

## Architecture

The filesystem subsystem follows a layered architecture:

**Userspace / Shell**
Applications and shell commands use the VFS API for all file operations.

**VFS Layer**
Provides a uniform interface for file operations. Dispatches calls to the appropriate filesystem driver via function pointers.

**FAT VFS Wrapper**
Converts VFS operations to FAT driver calls. Implements inode and file operations for FAT filesystems.

**FAT Driver**
Low-level FAT implementation. Manages clusters, directory entries, and the disk image in memory.

**Disk Image (Memory)**
The FAT image loaded by the bootloader, residing in physical memory.

**ATA Driver**
Only used during flush operations to write dirty sectors back to disk.

---

## FAT Driver

### Initialisation

The FAT driver is initialised by calling `fat_init()` with the memory address of the FAT image and its size. The function:

1. Parses the boot sector (BPB) to determine filesystem parameters.
2. Determines the FAT type (12, 16, or 32) based on the number of clusters.
3. Calculates the locations of the FAT tables, root directory, and data region.
4. Allocates a dirty sector bitmap for tracking modified sectors.
5. Stores all information in the `fat_fs_t` structure.

### Boot Sector Parsing

The driver reads the following fields from the boot sector:

| Field | Description |
|-------|-------------|
| `bytes_per_sector` | Usually 512 bytes. |
| `sectors_per_cluster` | Number of sectors in a cluster (1, 2, 4, 8, 16, 32, 64, 128). |
| `reserved_sectors` | Number of reserved sectors before the FAT. |
| `num_fats` | Number of FAT copies (usually 2). |
| `root_entries` | Number of root directory entries (FAT12/16 only). |
| `total_sectors_16` | Total sectors (16-bit field). |
| `total_sectors_32` | Total sectors (32-bit field, used if 16-bit field is 0). |
| `sectors_per_fat_16` | Sectors per FAT (16-bit field). |
| `sectors_per_fat_32` | Sectors per FAT (32-bit field, FAT32 only). |
| `media` | Media descriptor byte. |
| `root_cluster` | First cluster of the root directory (FAT32 only). |

### FAT Types

The FAT type is determined by the number of clusters:

| Type | Cluster Count |
|------|---------------|
| FAT12 | Less than 4085 |
| FAT16 | 4085 to 65524 |
| FAT32 | 65525 or more |

### Directory Operations

**Opening a Directory**
`fat_opendir()` initialises a directory iterator for the specified cluster (or root for FAT12/16).

**Reading Directory Entries**
`fat_readdir()` reads the next entry from the directory. It skips deleted entries (0xE5) and long file name entries (0x0F). Returns 1 on success, 0 at end of directory.

**Creating a Directory**
`fat_mkdir()` creates a new directory with the specified name. It allocates a cluster, initialises "." and ".." entries, and creates a directory entry in the parent.

**Removing a Directory**
`fat_rm()` removes a file or empty directory. It frees the cluster chain and marks the directory entry as deleted (0xE5). For directories, it checks that the directory is empty before deletion.

### File Operations

**Opening a File**
`fat_open()` finds a file in the root directory and returns its size.

**Reading a File**
`fat_read_file()` reads data from a file into a buffer. It follows the cluster chain and copies data until the requested size is reached or the end of file is encountered.

**Writing a File**
`fat_write_file()` overwrites or creates a file with the given data. It frees any existing cluster chain, allocates new clusters, and copies the data. If the file size is 0, it frees all clusters.

**Appending to a File**
`fat_append_file()` appends data to an existing file. It finds the last cluster, writes data starting from the current end, and allocates new clusters as needed.

**Creating a File**
`fat_create_file()` creates an empty file with a zero-length cluster chain.

### Cluster Management

**Reading FAT Entries**
`get_fat_entry()` reads a FAT entry for a given cluster. The entry indicates the next cluster in the chain or an end-of-chain marker.

**Writing FAT Entries**
`set_fat_entry()` writes a FAT entry and marks the corresponding FAT sector as dirty.

**End of Chain Detection**
`is_eoc()` checks if a cluster value indicates the end of a cluster chain.

**Allocating Clusters**
`find_free_cluster()` scans the FAT for a free cluster (entry value 0).

**Freeing Cluster Chains**
`free_cluster_chain()` marks all clusters in a chain as free.

### Dirty Tracking and Flushing

The FAT driver maintains a dirty sector bitmap to track which sectors have been modified. This allows the driver to efficiently write only changed sectors back to disk.

**Marking Sectors Dirty**
`fat_mark_sector_dirty()` sets a bit in the bitmap for the specified LBA.

**Flushing to Disk**
`fat_flush()` writes all dirty sectors back to the physical disk using the ATA driver. It compares memory sectors with disk sectors to avoid unnecessary writes.

The dirty bitmap is stored in the `fat_fs_t` structure and is allocated during initialisation.

---

## Virtual Filesystem (VFS)

### Core Concepts

**Inodes**
An inode represents a filesystem object (file, directory, or device). It contains:

- **inode number** – unique identifier within the filesystem.
- **type** – file, directory, character device, block device, pipe, or symlink.
- **size** – size of the object in bytes.
- **reference count** – number of open file descriptors referencing this inode.
- **private data** – filesystem-specific data (e.g., FAT cluster information).
- **operations** – function pointers for inode operations.

**Files**
A file represents an open file descriptor. It contains:

- **file descriptor number** – unique within the process.
- **inode** – pointer to the underlying inode.
- **offset** – current position within the file.
- **flags** – open mode (read, write, create, truncate, append).
- **operations** – function pointers for file operations.

### Inodes

Inodes are created by `vfs_create_inode()`, which allocates memory and initialises the structure. The inode number is typically the cluster number of the file (or 1 for the root directory).

### File Descriptors

File descriptors are allocated by `alloc_fd()`, which searches the current process's file table for an empty slot. The maximum number of file descriptors per process is 16.

### File Operations

File operations are function pointers that define how to perform operations on an open file:

| Operation | Description |
|-----------|-------------|
| `read` | Reads data from the file into a buffer. |
| `write` | Writes data from a buffer to the file. |
| `seek` | Moves the file offset. |
| `close` | Closes the file descriptor. |

### Inode Operations

Inode operations define how to perform operations on a filesystem object:

| Operation | Description |
|-----------|-------------|
| `lookup` | Finds a file or directory by name within a directory. |
| `create` | Creates a new file or directory. |
| `remove` | Deletes a file or directory. |
| `readdir` | Reads the next directory entry. |

### Per-Process File Tables

Each process has its own file descriptor table (`fd_table_t`). This allows processes to have independent file descriptors. The current process's table is pointed to by `current_fd_table`.

---

## FAT VFS Wrapper

The FAT VFS wrapper (`fat_vfs.c`) bridges the raw FAT driver and the VFS layer.

### Private Data

The wrapper defines `fat_private_t`, which is stored in `inode_t::private_data`. It contains:

- **cluster** – the first cluster of the file/directory.
- **entry** – the raw FAT directory entry.
- **is_dir** – whether the object is a directory.
- **dir** – a `fat_dir_t` iterator for directories.

### Path Resolution

`fat_lookup_path()` resolves a full path by walking through directory entries. It handles `.` and `..` correctly and returns the final directory entry and its cluster.

**Path Resolution Algorithm:**
1. Split the path into components.
2. Start from the root directory (cluster 0).
3. For each component, search the current directory for the name.
4. If the component is `..`, move to the parent directory.
5. If the component is `.`, skip it.
6. If a component is not found, return an error.
7. Return the final entry and its cluster.

### Parent Resolution

`fat_resolve_parent()` splits a path into the parent directory and the base name. This is used by `vfs_fat_create()` and `vfs_fat_mkdir()` to determine where to create a new object.

### Directory Iteration

The wrapper uses the FAT driver's directory iteration functions:
- `fat_opendir()` initialises a directory iterator.
- `fat_readdir()` reads the next entry.
- `fat_closedir()` closes the iterator.

The VFS `readdir` operation calls `fat_readdir()` and converts the FAT entry to a `vfs_dirent_t` structure with inode number, type, and name.

---

## Special Devices

The VFS supports a console device at `/dev/console` (or simply `console`). Writing to this device prints to the screen; reading returns 0.

The console device uses the `file_ops_t` structure with:
- `read` – returns 0 (no input).
- `write` – prints characters to the console.
- `seek` – returns -1 (not supported).
- `close` – does nothing.

Standard file descriptors (stdin, stdout, stderr) are connected to the console device.

---

## Dependencies

| Component | Depends On | Purpose |
|-----------|------------|---------|
| FAT Driver | Heap | Memory allocation for dirty bitmap |
| FAT Driver | ATA Driver | Flushing dirty sectors to disk |
| FAT Driver | Console | Error reporting and logging |
| VFS | Heap | Memory allocation for inodes and files |
| VFS | FAT Driver | Underlying filesystem operations |
| FAT VFS Wrapper | FAT Driver, VFS | Bridge between layers |
| Special Devices | Console | Output operations |

---

## Conclusion

The filesystem subsystem provides a solid foundation for persistent storage and device I/O. The separation between the low-level FAT driver and the generic VFS layer makes the system extensible and maintainable. The current implementation supports the core operations needed for a hobby operating system.

For more details, refer to the source code in `fs/fat/`, `fs/vfs/`, and the FAT VFS wrapper.

---

**Document Version:** 1.0  
**Last Updated:** September 2026  
**Project:** LufiraOS