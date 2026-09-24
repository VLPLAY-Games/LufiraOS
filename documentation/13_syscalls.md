# System Calls

This document describes the system call interface of LufiraOS. System calls provide a controlled mechanism for user-mode programs to request services from the kernel.

---

## Table of Contents

1. [Overview](#overview)
2. [System Call Mechanism](#system-call-mechanism)
   - [MSR Configuration](#msr-configuration)
   - [System Call Entry Stub](#system-call-entry-stub)
   - [System Call Handler](#system-call-handler)
3. [User Pointer Validation](#user-pointer-validation)
4. [System Call Table](#system-call-table)
5. [System Call Descriptions](#system-call-descriptions)
   - [File Operations](#file-operations)
   - [Memory Management](#memory-management)
   - [Process Operations](#process-operations)
   - [Filesystem / Identity](#filesystem--identity)
   - [System Information](#system-information)
6. [Calling Convention](#calling-convention)
7. [Error Handling](#error-handling)
8. [Dependencies](#dependencies)

---

## Overview

System calls provide the interface between user-mode programs and the kernel. They allow user programs to:

- Perform file and directory operations (open, close, read, write, seek, pipe, mkdir, rmdir, unlink, readdir).
- Manage memory (`mmap`/`munmap`, anonymous only).
- Manage processes (fork, exec with real `argv`/`envp`, wait, kill/signals, sleep).
- Query and change identity/permissions (getuid, getgid, chmod, chown) and the working directory (getcwd, chdir).
- Query system information (PID, timer ticks).
- Control system behaviour (yield CPU, exit).

**Design Philosophy:**
- **Efficiency** – uses the `syscall` instruction for fast transitions.
- **Simplicity** – the calling convention matches the x86-64 ABI.
- **Safety** – every syscall that dereferences a user-supplied pointer validates it against the calling process's own page tables before touching it, and reports a specific error code instead of trusting or silently rejecting.

---

## System Call Mechanism

### MSR Configuration

The `syscall` instruction is configured using Model-Specific Registers (MSRs):

| MSR | Value | Description |
|-----|-------|-------------|
| `IA32_STAR` (0xC0000081) | Kernel CS (bits 47:32), User CS (bits 63:48) | Defines segment selectors for transitions. |
| `IA32_LSTAR` (0xC0000082) | Address of `syscall_entry` | The entry point for system calls. |
| `IA32_FMASK` (0xC0000084) | 0x200 | Clears the interrupt flag on entry. |
| `IA32_EFER` (0xC0000080) | SCE bit (bit 0) | Enables the `syscall` instruction. |

### System Call Entry Stub

The entry stub (`syscall_entry.S`) is written in assembly and handles the transition from user mode to kernel mode:

1. **Save User RSP** – the user stack pointer is saved to a global variable.
2. **Switch to Kernel Stack** – load `current_kernel_rsp` (from the current process).
3. **Save User Context** – push user RIP, RFLAGS, and all registers.
4. **Call the Handler** – call `syscall_handler()` with the system call number and arguments.
5. **Restore Context** – restore registers from the stack.
6. **Return to User Mode** – restore user RSP and execute `iretq`.

### System Call Handler

The handler (`syscall_handler()`) is a C function that:

1. Validates the system call number (0–255).
2. Special-cases `SYS_FORK` (it needs a pointer to the whole saved register frame, not just the usual five arguments) rather than dispatching it through `syscall_table[]`.
3. Looks up the function pointer in the system call table for everything else.
4. Calls the function with the provided arguments.
5. Returns the result to the user program.

**Prototype:** `uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)`

Since a syscall handler always runs in ring 0, the timer's preemption check (`frame->cs == 0x33`, see [`10_cpu_interrupts.md`](10_cpu_interrupts.md#preemption)) never fires mid-syscall — no syscall body needs to guard against being preempted partway through.

---

## User Pointer Validation

`is_user_accessible()`/`is_user_range_valid()` (`system/mm/paging.c`) walk the calling process's own page tables and require the `PAGE_USER` bit set at **every** translation level for the whole requested range — not just a "is this address low/canonical" bounds check, since the kernel's own identity-mapped code/data lives in the same low virtual range as user memory and only the permission bits tell them apart. `validate_user_string()` (`syscall.c`) additionally scans for a NUL terminator page-by-page, bounded by `USER_STRING_MAX` (4096 bytes).

Every syscall that touches a user buffer or string calls one of these first and returns `-EFAULT` on failure, before doing anything else — including before a blocking call like `SYS_WAIT` would otherwise block on a bad pointer.

---

## System Call Table

The system call table is an array of function pointers indexed by system call number.

| Number | Name | Description |
|--------|------|-------------|
| 0 | `SYS_WRITE` | Write to a file descriptor |
| 1 | `SYS_READ` | Read from a file descriptor |
| 2 | `SYS_EXIT` | Terminate the current process |
| 3 | `SYS_GETPID` | Get the current process ID |
| 4 | `SYS_YIELD` | Yield the CPU |
| 5 | `SYS_GETTICK` | Get timer ticks since boot |
| 6 | `SYS_OPEN` | Open a file |
| 7 | `SYS_CLOSE` | Close a file descriptor |
| 8 | `SYS_SEEK` | Reposition file offset |
| 9 | `SYS_MMAP` | Map anonymous memory |
| 10 | `SYS_MUNMAP` | Unmap memory |
| 11 | `SYS_EXEC` | Replace the current process image with a new program |
| 12 | `SYS_FORK` | Create a child process |
| 13 | `SYS_WAIT` | Wait for a child process |
| 14 | `SYS_GETCWD` | Get current working directory |
| 15 | `SYS_CHDIR` | Change current directory |
| 16 | `SYS_SLEEP` | Sleep for milliseconds |
| 17 | `SYS_KILL` | Send a signal to a process |
| 18 | `SYS_PIPE` | Create an anonymous pipe |
| 19 | `SYS_CHMOD` | Change a file's permission bits |
| 20 | `SYS_CHOWN` | Change a file's owner/group (root only) |
| 21 | `SYS_GETUID` | Get the calling process's user ID |
| 22 | `SYS_GETGID` | Get the calling process's group ID |
| 23 | `SYS_MKDIR` | Create a directory |
| 24 | `SYS_RMDIR` | Remove a directory |
| 25 | `SYS_UNLINK` | Remove a file |
| 26 | `SYS_READDIR` | Read the next directory entry from an open directory fd |

All 27 are implemented — there are no remaining stubs.

---

## System Call Descriptions

### File Operations

**SYS_WRITE (0)**
- **Signature:** `int write(int fd, const void *buf, size_t count)`
- **Description:** Writes up to `count` bytes from `buf` to the file descriptor `fd`.
- **Returns:** Number of bytes written, `-EFAULT` if `buf` isn't a valid, fully-mapped user range.
- **Implementation:** Validates `buf` via `is_user_range_valid()` (read-only), then calls `vfs_write()`.

**SYS_READ (1)**
- **Signature:** `int read(int fd, void *buf, size_t count)`
- **Description:** Reads up to `count` bytes from `fd` into `buf`.
- **Returns:** Number of bytes read, `-EFAULT` if `buf` isn't valid/writable.
- **Implementation:** Validates `buf` via `is_user_range_valid()` (writable), then calls `vfs_read()`.

**SYS_OPEN (6)**
- **Signature:** `int open(const char *path, int flags, int mode)`
- **Description:** Opens a file specified by `path` (resolved from the LufiraFS root, not the cwd — the older of two resolution conventions in this codebase, see [`08_filesystem.md`](08_filesystem.md#path-resolution)). Checks read/write permission on the target inode, or write+exec on the parent directory when `O_CREAT` is creating a new entry.
- **Returns:** File descriptor, `-EFAULT` for a bad `path` pointer, `-EACCES` on a permission check failure, `-1` for other VFS-level failures.
- **Implementation:** `lufirafs_check_access()` then `vfs_open()`.

**SYS_CLOSE (7)**
- **Signature:** `int close(int fd)`
- **Description:** Closes a file descriptor.
- **Returns:** `0` on success, `-1` on error.
- **Implementation:** Calls `vfs_close()`.

**SYS_SEEK (8)**
- **Signature:** `off_t seek(int fd, off_t offset, int whence)`
- **Description:** Repositions the file offset.
- **Returns:** New offset, or `-1` on error.
- **Implementation:** Calls `vfs_seek()`.

**SYS_PIPE (18)**
- **Signature:** `int pipe(int fds[2])`
- **Description:** Creates an anonymous pipe; `fds[0]` is the read end, `fds[1]` the write end, both in the calling process's own file descriptor table.
- **Returns:** `0` on success, `-EFAULT` for a bad `fds` pointer, `-1` on other errors.
- **Implementation:** Validates `fds` (`2 * sizeof(int)`, writable), then calls `vfs_pipe()`.

### Memory Management

**SYS_MMAP (9)**
- **Signature:** `void *mmap(void *addr, size_t length, int prot, int flags, int fd)`
- **Description:** Maps `length` bytes of zero-filled anonymous memory. `MAP_ANONYMOUS` is required; `MAP_FIXED` and file-backed mappings (a real `fd`) are not supported. `addr`/`fd` are ignored. Pages are allocated and mapped **eagerly** — there is no demand-paging path — with `PROT_EXEC` absent mapping to the NX bit. Up to `MAX_MMAP_REGIONS` (32) concurrent regions per process; the virtual address space is a per-process bump allocator starting at `MMAP_AREA_START` that never reclaims space after `munmap()`.
- **Returns:** Base address of the new mapping, or `(uint64_t)-1` on failure (bad flags, no free region slot, or out of physical memory — in which case any pages already mapped for this call are rolled back).
- **Implementation:** `pmm_alloc_page()` + `map_page_in_pml4()` per page, in `sys_mmap()`.

**SYS_MUNMAP (10)**
- **Signature:** `int munmap(void *addr, size_t length)`
- **Description:** Unmaps a region. `addr`/`length` must **exactly** match a region previously returned by `mmap()` — there is no partial/sub-range unmap.
- **Returns:** `0` on success, `-1` if no exact match was found.
- **Implementation:** `unmap_page()` per page (which also frees the physical frame), in `sys_munmap()`.

### Process Operations

**SYS_EXIT (2)**
- **Signature:** `void exit(int status)`
- **Description:** Terminates the current process.
- **Returns:** Does not return.
- **Implementation:** Calls `process_exit()`.

**SYS_GETPID (3)**
- **Signature:** `pid_t getpid(void)`
- **Description:** Returns the current process ID.
- **Returns:** PID of the current process.
- **Implementation:** Returns `current_process->pid`.

**SYS_YIELD (4)**
- **Signature:** `void yield(void)`
- **Description:** Yields the CPU to another process. With preemptive scheduling ([`10_cpu_interrupts.md`](10_cpu_interrupts.md#preemption)) this is no longer the only way to give up the CPU, but it still forces an immediate reschedule instead of waiting for the next timer tick.
- **Returns:** `0`.
- **Implementation:** Calls `schedule()`.

**SYS_SLEEP (16)**
- **Signature:** `unsigned int sleep(unsigned int milliseconds)`
- **Description:** Sleeps for `milliseconds`.
- **Returns:** `0` (always).
- **Implementation:** Calls `process_sleep(milliseconds)`.

**SYS_FORK (12)**
- **Signature:** `pid_t fork(void)`
- **Description:** Duplicates the calling process (address space, file descriptors, cwd, `uid`/`gid`). Handled specially in `syscall_handler()` rather than through `syscall_table[]`, since it needs a pointer to the entire saved register frame.
- **Returns:** Child PID to the parent, `0` to the child, `(uint64_t)-1` on failure (out of memory while cloning the address space).
- **Implementation:** Calls `process_fork(frame_ptr)`. See [`12_elf_processes.md`](12_elf_processes.md#fork).

**SYS_EXEC (11)**
- **Signature:** `int exec(const char *filename, char *const argv[], char *const envp[])`
- **Description:** Replaces the calling process's image with a new ELF program, in place (same PID). `argv`/`envp` are copied out of the caller's memory (validated, bounded by `MAX_EXEC_ARGS`/`MAX_EXEC_ARGS_BYTES`) and genuinely passed to the new program's `main()` — see [`12_elf_processes.md`](12_elf_processes.md#argv--envp).
- **Returns:** Does not return on success; `-EFAULT` for a bad `filename`/`argv`/`envp` pointer, `-1` if the file can't be found/read or the exec bit is not set.
- **Implementation:** `copy_user_string_array()` for `argv`/`envp`, then `do_exec()` → `elf_exec_replace()`.

**SYS_WAIT (13)**
- **Signature:** `pid_t wait(pid_t pid, int *status)`
- **Description:** Blocks until the given child (`pid == 0` = any child) terminates. `status` may be `NULL`.
- **Returns:** The reaped child's PID, `-EFAULT` if `status` is non-NULL but invalid, `-1` if the caller has no such child.
- **Implementation:** Validates `status` (if given) before blocking, then calls `process_wait()`.

**SYS_KILL (17)**
- **Signature:** `int kill(pid_t pid, int sig)`
- **Description:** Sends a signal to a process (`sig == 0` defaults to `SIGTERM`). Only default actions are implemented — no user-space signal handlers.
- **Returns:** `0` on success, `-1` if the process was not found.
- **Implementation:** Calls `process_signal(pid, sig)`.

### Filesystem / Identity

**SYS_GETCWD (14)**
- **Signature:** `char *getcwd(char *buffer, size_t size)`
- **Description:** Copies the calling process's current working directory (with NUL) into `buffer`.
- **Returns:** Length of the path (excluding NUL) on success; `-EFAULT` for a bad buffer, `-EINVAL` if `size == 0`, `-ERANGE` if the path doesn't fit.
- **Implementation:** `sys_getcwd()`, reads `current_process->cwd_path` directly (per-process state, not a shell global — see [`14_shell_commands.md`](14_shell_commands.md#current-directory)).

**SYS_CHDIR (15)**
- **Signature:** `int chdir(const char *path)`
- **Description:** Changes the current working directory, resolved relative to the process's own `cwd_inode` (not the LufiraFS root — the newer of the two path-resolution conventions, shared with `SYS_CHMOD`/`SYS_CHOWN`/`SYS_MKDIR`/`SYS_RMDIR`/`SYS_UNLINK`).
- **Returns:** `0` on success, `-EFAULT` for a bad pointer, `-EINVAL` for an empty path, `-ENOENT` if not found, `-ENOTDIR` if `path` isn't a directory.
- **Implementation:** `lufirafs_lookup()` + `lufirafs_get_path()`, updates `current_process->cwd_inode`/`cwd_path`.

**SYS_CHMOD (19)**
- **Signature:** `int chmod(const char *path, mode_t mode)`
- **Description:** Sets a file's 9-bit permission mode (e.g. `0644`). Only the file's owner or root may do this.
- **Returns:** `0` on success, `-EFAULT`/`-EINVAL` for a bad path, `-ENOENT` if not found, `-EPERM` if the caller is neither the owner nor root.
- **Implementation:** `sys_chmod()`, writes the inode's `perm` field back via `lufirafs_write_inode()` + `lufirafs_sync()`.

**SYS_CHOWN (20)**
- **Signature:** `int chown(const char *path, uid_t uid, gid_t gid)`
- **Description:** Changes a file's owner and group. Root only — no POSIX "owner may change to their own group" carve-out.
- **Returns:** `0` on success, `-EPERM` if the caller isn't root, `-ENOENT` if not found.
- **Implementation:** `sys_chown()`.

**SYS_GETUID (21) / SYS_GETGID (22)**
- **Signature:** `uid_t getuid(void)` / `gid_t getgid(void)`
- **Description:** Return the calling process's identity. No arguments.
- **Returns:** `current_process->uid` / `->gid`.

**SYS_MKDIR (23)**
- **Signature:** `int mkdir(const char *path, mode_t mode)`
- **Description:** Creates a directory, resolved relative to `cwd_inode`. `mode` is accepted (POSIX signature compatibility) but currently ignored — new directories always get LufiraFS's fixed default permission. Requires write+exec on the parent directory.
- **Returns:** `0` on success, `-EACCES` on a parent permission failure, `-ENOENT` if the parent doesn't resolve, `-1` on other VFS failures.
- **Implementation:** `lufirafs_check_access()` on the parent, then `vfs_mkdir_at()`.

**SYS_RMDIR (24) / SYS_UNLINK (25)**
- **Signature:** `int rmdir(const char *path)` / `int unlink(const char *path)`
- **Description:** Remove a directory / a file, resolved relative to `cwd_inode`. LufiraFS doesn't distinguish the two at the `lufirafs_unlink()` level, so both syscalls share one internal helper (`sys_remove()`) — neither checks that `path` is actually the right kind of entry. Requires write+exec on the parent directory.
- **Returns:** `0` on success, `-EACCES` on a parent permission failure, `-ENOENT` if the parent doesn't resolve, `-1` on other VFS failures.
- **Implementation:** `sys_remove()` → `vfs_rmdir_at()` / `vfs_unlink_at()`.

**SYS_READDIR (26)**
- **Signature:** `int readdir(int fd, struct lufira_dirent *out)`
- **Description:** Reads the next entry from a directory previously opened with `SYS_OPEN`. No separate permission check — read access was already verified when the directory was opened.
- **Returns:** `1` with `*out` filled in, `0` at end of directory, `-EFAULT` for a bad `out` pointer, `-1` on other errors.
- **Implementation:** Validates `out` (`sizeof(vfs_dirent_t)`, writable), then calls `vfs_readdir()`.

### System Information

**SYS_GETTICK (5)**
- **Signature:** `uint64_t gettick(void)`
- **Description:** Returns the number of timer ticks since boot.
- **Returns:** Timer tick count.
- **Implementation:** Returns `pit_get_ticks()`.

---

## Calling Convention

User-mode programs must follow the x86-64 ABI for system calls:

**Registers:**

| Register | Purpose |
|----------|---------|
| `RAX` | System call number |
| `RDI` | Argument 1 |
| `RSI` | Argument 2 |
| `RDX` | Argument 3 |
| `R10` | Argument 4 |
| `R8` | Argument 5 |
| `R9` | Argument 6 (accepted by the entry stub, not currently wired to any syscall's own dispatch — every implemented syscall takes 5 or fewer arguments) |
| `RCX` | Clobbered (saved RIP) |
| `R11` | Clobbered (saved RFLAGS) |

**Return Value:**
- `RAX` contains the return value.
- Errors are returned as `(uint64_t)-CODE` (two's-complement, the same convention used throughout this kernel) — reinterpreting the result as `int64_t` and checking `< 0` distinguishes an error from every legitimate success value (byte counts, `mmap` addresses in the high canonical user range, etc.).

## Error Handling

| Code | Value | Meaning |
|------|-------|---------|
| `EPERM` | 1 | Operation not permitted (e.g. `chmod`/`chown` by a non-owner/non-root). |
| `ENOENT` | 2 | No such file or directory. |
| `EACCES` | 13 | Permission denied (owner/group/other bits). |
| `EFAULT` | 14 | Bad user pointer (unmapped, or not accessible from ring 3). |
| `ENOTDIR` | 20 | Not a directory. |
| `EINVAL` | 22 | Invalid argument (e.g. an empty path string). |
| `ERANGE` | 34 | Result doesn't fit in the caller's buffer (`SYS_GETCWD`). |

Not every syscall failure carries one of these — some VFS-level failures (bad file descriptor, an underlying LufiraFS error with no distinct code of its own) still collapse to a bare `-1`, since the VFS layer itself has no error-code system yet. New syscalls added in this release (`SYS_CHMOD` onward) and every syscall that added pointer validation return one of the codes above specifically for that failure; older syscalls that predate this work (`SYS_OPEN`, `SYS_READ`/`SYS_WRITE`, …) mix specific codes for their new checks with bare `-1` for their pre-existing ones.

---

## Dependencies

| Component | Depends On | Purpose |
|-----------|------------|---------|
| System Calls | GDT, TSS | User/kernel mode transitions. |
| System Calls | Process Manager | Process operations, per-process `uid`/`gid`/cwd/mmap-region state. |
| System Calls | Paging | User-pointer validation (`is_user_range_valid()`). |
| System Calls | VFS / LufiraFS | File, directory, and permission operations. |
| System Calls | PIT | Timer-related operations. |
| System Calls | Console | (For debugging only). |

---

## Conclusion

All 27 defined system calls are implemented, with user-pointer validation and errno-style error codes covering process control (`fork`/`exec` with real `argv`/`envp`/`wait`/signals/pipes), anonymous memory mapping, file and directory I/O, and identity/permission management. The use of the `syscall` instruction ensures fast transitions, and the calling convention follows the x86-64 ABI for compatibility.

For more details, refer to the source code in `system/syscall/`.

---

**Document Version:** 2.0
**Last Updated:** September 2026
**Project:** LufiraOS
