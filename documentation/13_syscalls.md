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
   - [Privileged System Control (31–40)](#privileged-system-control-3140)
   - [USB Mass Storage (41–44)](#usb-mass-storage-4144)
   - [Polling, Signals, Alarms (45–49)](#polling-signals-alarms-4549)
   - [GUI / Window Manager (50–62)](#gui--window-manager-5062)
   - [Generic IPC (57–59)](#generic-ipc-5759)
   - [Miscellaneous (65–66)](#miscellaneous-6566)
6. [Calling Convention](#calling-convention)
7. [Error Handling](#error-handling)
8. [Dependencies](#dependencies)
9. [Conclusion](#conclusion)

---

## Overview

System calls provide the interface between user-mode programs and the kernel. As of v0.8.0 there are **65 implemented syscalls** (numbers 0–62, 65–66; 63 and 64 are explicitly retired and unused — see [System Call Table](#system-call-table)), up from 27 at v0.6.0. They let user programs:

- Perform file and directory operations (open, close, read, write, seek, pipe, mkdir, rmdir, unlink, readdir, dup2).
- Manage memory (`mmap`/`munmap`, anonymous only).
- Manage processes (fork, exec with real `argv`/`envp`, wait, kill/signals, sleep, foreground tracking for Ctrl+C).
- Query and change identity/permissions (getuid, getgid, chmod, chown, su, useradd, groupadd, passwd) and the working directory (getcwd, chdir).
- Query system information (PID, timer ticks, memory/CPU/process stats).
- Drive privileged system control (mount/unmount, reboot, shutdown, developer mode).
- Talk to USB Mass Storage devices directly (count/info/read/write).
- Build and drive a GUI: create/manipulate windows, poll input events, register as the window manager, and present framebuffer contents (syscalls 50–62).
- Exchange small messages between arbitrary (not just related) processes via a generic per-process mailbox (`SYS_IPC_SEND`/`SYS_IPC_RECV`).
- Fetch a URL over the network in one blocking call, including DNS, TCP/TLS, and HTTP response parsing (`SYS_NET_FETCH`).
- Control system behaviour (yield CPU, exit).

**Design Philosophy:**
- **Efficiency** – uses the `syscall` instruction for fast transitions.
- **Simplicity** – the calling convention matches the x86-64 ABI.
- **Safety** – every syscall that dereferences a user-supplied pointer validates it against the calling process's own page tables before touching it, and reports a specific error code instead of trusting or silently rejecting.
- **Push functionality out of the kernel, not into it** – nearly every syscall added after v0.6.0 exists specifically to let something that used to be kernel-native code (the shell, `su`/`useradd`/`mount`/`reboot`, the window manager) move into an ordinary userspace process instead. The syscall surface grew so that the kernel itself could shrink — see [`14_shell_commands.md`](14_shell_commands.md), [`18_gui_wm.md`](18_gui_wm.md).

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
2. Special-cases `SYS_FORK` and `SYS_SIGRETURN` (both need a pointer to the whole saved register frame rather than the usual five arguments — `SYS_FORK` to construct the child's saved context, `SYS_SIGRETURN` to restore the exact state a signal handler interrupted) rather than dispatching them through `syscall_table[]`.
3. Looks up the function pointer in the system call table for everything else.
4. Calls the function with the provided arguments.
5. Returns the result to the user program.

**Prototype:** `uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5, uint64_t frame_ptr)`

Since a syscall handler always runs in ring 0, the timer's preemption check (`frame->cs == 0x33`, see [`10_cpu_interrupts.md`](10_cpu_interrupts.md#preemption)) never fires mid-syscall — no syscall body needs to guard against being preempted partway through.

---

## User Pointer Validation

`is_user_accessible()`/`is_user_range_valid()` (`system/mm/paging.c`) walk the calling process's own page tables and require the `PAGE_USER` bit set at **every** translation level for the whole requested range — not just a "is this address low/canonical" bounds check, since the kernel's own identity-mapped code/data lives in the same low virtual range as user memory and only the permission bits tell them apart. `validate_user_string()` (`syscall.c`) additionally scans for a NUL terminator page-by-page, bounded by `USER_STRING_MAX` (4096 bytes).

Every syscall that touches a user buffer or string calls one of these first and returns `-EFAULT` on failure, before doing anything else — including before a blocking call like `SYS_WAIT`/`SYS_IPC_RECV`/`SYS_NET_FETCH` would otherwise block on a bad pointer.

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
| 27 | `SYS_STATFS` | Filesystem usage statistics |
| 28 | `SYS_MEMINFO` | Physical memory / kernel heap statistics |
| 29 | `SYS_CPULOAD` | Raw PIT tick counters for CPU-load sampling |
| 30 | `SYS_PSLIST` | Snapshot the process table |
| 31 | `SYS_SET_FOREGROUND` | Mark a child PID as foreground (for Ctrl+C delivery) |
| 32 | `SYS_SU` | Switch the caller's own uid/gid after a kernel-side password check |
| 33 | `SYS_MOUNT` | Mount a USB Mass Storage device's FAT filesystem under a VFS prefix |
| 34 | `SYS_UNMOUNT` | Flush and unmount a FAT prefix |
| 35 | `SYS_REBOOT` | Reboot the machine (root only) |
| 36 | `SYS_SHUTDOWN` | Shut the machine down (root only) |
| 37 | `SYS_DEVMODE` | Query/enable/disable developer mode |
| 38 | `SYS_USERADD` | Create a new user account (root only) |
| 39 | `SYS_GROUPADD` | Create a new group (root only) |
| 40 | `SYS_PASSWD` | Change a password (own, or any user's if root) |
| 41 | `SYS_USB_COUNT` | Number of detected USB Mass Storage devices |
| 42 | `SYS_USB_INFO` | Geometry (max LBA, block size) of a USB MSD device |
| 43 | `SYS_USB_READ` | Read one raw block from a USB MSD device |
| 44 | `SYS_USB_WRITE` | Write one raw block to a USB MSD device (root only) |
| 45 | `SYS_POLL` | Poll multiple file descriptors at once |
| 46 | `SYS_SIGACTION` | Install a SIGINT/SIGTERM handler |
| 47 | `SYS_SIGRETURN` | Return from a signal handler (special-cased, see above) |
| 48 | `SYS_ALARM` | Arm/disarm a one-shot SIGALRM timer |
| 49 | `SYS_GET_FOREGROUND` | Read the current foreground PID |
| 50 | `SYS_WIN_CREATE` | Create a GUI window |
| 51 | `SYS_WIN_DESTROY` | Destroy a GUI window |
| 52 | `SYS_WIN_FILL` | Fill a window's client area with one color |
| 53 | `SYS_WIN_DRAW_RECT` | Draw a filled rectangle into a window |
| 54 | `SYS_WIN_DRAW_TEXT` | Draw bitmap-font text into a window |
| 55 | `SYS_WIN_POLL_EVENT` | Poll one window's event queue |
| 56 | `SYS_WIN_MOVE` | Move a window |
| 57 | `SYS_IPC_SEND` | Send a message to another process's mailbox |
| 58 | `SYS_IPC_RECV` | Receive a message from the caller's own mailbox |
| 59 | `SYS_WM_REGISTER` | Register the caller as the system's one window manager |
| 60 | `SYS_FB_INFO` | Query framebuffer dimensions/pixel format |
| 61 | `SYS_FB_PRESENT` | Present a composited frame (window-manager only) |
| 62 | `SYS_FB_FONT` | Fetch the kernel's built-in 8x8 bitmap font |
| 63 | *(retired)* | `SYS_CONSOLE_INJECT` — removed in v0.8 stage 4; number intentionally left unused, not reassigned. |
| 64 | *(retired)* | `SYS_CONSOLE_REDRAW` — removed in v0.8 stage 4; number intentionally left unused, not reassigned. |
| 65 | `SYS_DUP2` | Duplicate a file descriptor onto a specific slot |
| 66 | `SYS_NET_FETCH` | Fetch a URL (http/https) in one blocking call |

All 65 are implemented — there are no remaining stubs. 63 and 64 are not stubs either; they are permanently retired (see their description under [Miscellaneous](#miscellaneous-6566)).

---

## System Call Descriptions

### File Operations

**SYS_WRITE (0)**
- **Signature:** `long sys_write(int fd, const void *buf, unsigned long count)`
- **Description:** Writes up to `count` bytes from `buf` to the file descriptor `fd`.
- **Returns:** Number of bytes written, `-EFAULT` if `buf` isn't a valid, fully-mapped user range.
- **Implementation:** Validates `buf` via `is_user_range_valid()` (read-only), then calls `vfs_write()`.

**SYS_READ (1)**
- **Signature:** `long sys_read(int fd, void *buf, unsigned long count)`
- **Description:** Reads up to `count` bytes from `fd` into `buf`.
- **Returns:** Number of bytes read, `-EFAULT` if `buf` isn't valid/writable.
- **Implementation:** Validates `buf` via `is_user_range_valid()` (writable), then calls `vfs_read()`.

**SYS_OPEN (6)**
- **Signature:** `long sys_open(const char *path, int flags, int mode)`
- **Description:** Opens a file specified by `path` (resolved from the LufiraFS root, not the cwd — the older of two resolution conventions in this codebase, see [`08_filesystem.md`](08_filesystem.md#path-resolution)). Checks read/write permission on the target inode, or write+exec on the parent directory when `O_CREAT` is creating a new entry.
- **Returns:** File descriptor, `-EFAULT` for a bad `path` pointer, `-EACCES` on a permission check failure, `-1` for other VFS-level failures.
- **Implementation:** `lufirafs_check_access()` then `vfs_open()`.

**SYS_CLOSE (7)**
- **Signature:** `long sys_close(int fd)`
- **Description:** Closes a file descriptor.
- **Returns:** `0` on success, `-1` on error.
- **Implementation:** Calls `vfs_close()`.

**SYS_SEEK (8)**
- **Signature:** `long sys_lseek(int fd, long offset, int whence)`
- **Description:** Repositions the file offset.
- **Returns:** New offset, or `-1` on error.
- **Implementation:** Calls `vfs_seek()`.

**SYS_PIPE (18)**
- **Signature:** `long sys_pipe(int fds[2])`
- **Description:** Creates an anonymous pipe; `fds[0]` is the read end, `fds[1]` the write end, both in the calling process's own file descriptor table.
- **Returns:** `0` on success, `-EFAULT` for a bad `fds` pointer, `-1` on other errors.
- **Implementation:** Validates `fds` (`2 * sizeof(int)`, writable), then calls `vfs_pipe()`.

### Memory Management

**SYS_MMAP (9)**
- **Signature:** `long sys_mmap(void *addr, unsigned long length, int prot, int flags, int fd)`
- **Description:** Maps `length` bytes of zero-filled anonymous memory. `MAP_ANONYMOUS` is required; `MAP_FIXED` and file-backed mappings (a real `fd`) are not supported. `addr`/`fd` are ignored. Pages are allocated and mapped **eagerly** — there is no demand-paging path — with `PROT_EXEC` absent mapping to the NX bit. Up to `MAX_MMAP_REGIONS` (32) concurrent regions per process; the virtual address space is a per-process bump allocator starting at `MMAP_AREA_START` that never reclaims space after `munmap()`.
- **Returns:** Base address of the new mapping, or `(uint64_t)-1` on failure (bad flags, no free region slot, or out of physical memory — in which case any pages already mapped for this call are rolled back).
- **Implementation:** `pmm_alloc_page()` + `map_page_in_pml4()` per page, in `sys_mmap()`.

**SYS_MUNMAP (10)**
- **Signature:** `long sys_munmap(void *addr, unsigned long length)`
- **Description:** Unmaps a region. `addr`/`length` must **exactly** match a region previously returned by `mmap()` — there is no partial/sub-range unmap.
- **Returns:** `0` on success, `-1` if no exact match was found.
- **Implementation:** `unmap_page()` per page (which also frees the physical frame), in `sys_munmap()`.

### Process Operations

**SYS_EXIT (2)**
- **Signature:** `void sys_exit(int status)` — noreturn
- **Description:** Terminates the current process.
- **Returns:** Does not return.
- **Implementation:** Calls `process_exit()`.

**SYS_GETPID (3)**
- **Signature:** `long sys_getpid(void)`
- **Description:** Returns the current process ID.
- **Returns:** PID of the current process.
- **Implementation:** Returns `current_process->pid`.

**SYS_YIELD (4)**
- **Signature:** `long sys_yield(void)`
- **Description:** Yields the CPU to another process. With preemptive scheduling ([`10_cpu_interrupts.md`](10_cpu_interrupts.md#preemption)) this is no longer the only way to give up the CPU, but it still forces an immediate reschedule instead of waiting for the next timer tick.
- **Returns:** `0`.
- **Implementation:** Calls `schedule()`.

**SYS_SLEEP (16)**
- **Signature:** `long sys_msleep(unsigned long milliseconds)`
- **Description:** Sleeps for `milliseconds`.
- **Returns:** `0` (always).
- **Implementation:** Calls `process_sleep(milliseconds)`.

**SYS_FORK (12)**
- **Signature:** `long sys_fork(void)`
- **Description:** Duplicates the calling process (address space, file descriptors, cwd, `uid`/`gid`). Handled specially in `syscall_handler()` rather than through `syscall_table[]`, since it needs a pointer to the entire saved register frame.
- **Returns:** Child PID to the parent, `0` to the child, `(uint64_t)-1` on failure (out of memory while cloning the address space).
- **Implementation:** Calls `process_fork(frame_ptr)`. See [`12_elf_processes.md`](12_elf_processes.md#fork).

**SYS_EXEC (11)**
- **Signature:** `long sys_exec(const char *filename, char *const argv[], char *const envp[])`
- **Description:** Replaces the calling process's image with a new ELF program, in place (same PID). `argv`/`envp` are copied out of the caller's memory (validated, bounded by `MAX_EXEC_ARGS`/`MAX_EXEC_ARGS_BYTES`) and genuinely passed to the new program's `main()` — see [`12_elf_processes.md`](12_elf_processes.md#command-line-arguments-argvenvp). If the target links against `/lib/libc.so`, dynamic linking is resolved as part of the load — see [`12_elf_processes.md`](12_elf_processes.md#dynamic-linking-libcso).
- **Returns:** Does not return on success; `-EFAULT` for a bad `filename`/`argv`/`envp` pointer, `-1` if the file can't be found/read or the exec bit is not set.
- **Implementation:** `copy_user_string_array()` for `argv`/`envp`, then `do_exec()` → `elf_exec_replace()`.

**SYS_WAIT (13)**
- **Signature:** `long sys_wait(long pid, int *status, int options)`
- **Description:** Blocks until the given child (`pid == 0` = any child) terminates. `status` may be `NULL`.
- **Returns:** The reaped child's PID, `-EFAULT` if `status` is non-NULL but invalid, `-1` if the caller has no such child.
- **Implementation:** Validates `status` (if given) before blocking, then calls `process_wait()`.

**SYS_KILL (17)**
- **Signature:** `long sys_kill(long pid, int sig)`
- **Description:** Sends a signal to a process (`sig == 0` defaults to `SIGTERM`). `SIGINT`/`SIGTERM` can be caught via `SYS_SIGACTION` (see [Polling, Signals, Alarms](#polling-signals-alarms-4549)); other signals still only have their default action.
- **Returns:** `0` on success, `-1` if the process was not found.
- **Implementation:** Calls `process_signal(pid, sig)`.

### Filesystem / Identity

**SYS_GETCWD (14)**
- **Signature:** `long sys_getcwd(char *buf, unsigned long size)`
- **Description:** Copies the calling process's current working directory (with NUL) into `buf`.
- **Returns:** Length of the path (excluding NUL) on success; `-EFAULT` for a bad buffer, `-EINVAL` if `size == 0`, `-ERANGE` if the path doesn't fit.
- **Implementation:** `sys_getcwd()`, reads `current_process->cwd_path` directly (per-process state, not a shell global — see [`14_shell_commands.md`](14_shell_commands.md)).

**SYS_CHDIR (15)**
- **Signature:** `long sys_chdir(const char *path)`
- **Description:** Changes the current working directory, resolved relative to the process's own `cwd_inode` (not the LufiraFS root — the newer of the two path-resolution conventions, shared with `SYS_CHMOD`/`SYS_CHOWN`/`SYS_MKDIR`/`SYS_RMDIR`/`SYS_UNLINK`). Also understands a mounted FAT prefix (`SYS_MOUNT`) transparently. Does **not** check the target directory's exec/search permission bit at all (see [`15_users_permissions.md`](15_users_permissions.md#known-limitations)).
- **Returns:** `0` on success, `-EFAULT` for a bad pointer, `-EINVAL` for an empty path, `-ENOENT` if not found, `-ENOTDIR` if `path` isn't a directory.
- **Implementation:** `lufirafs_lookup()` + `lufirafs_get_path()`, updates `current_process->cwd_inode`/`cwd_path`.

**SYS_CHMOD (19)**
- **Signature:** `long sys_chmod(const char *path, int mode)`
- **Description:** Sets a file's 9-bit permission mode (e.g. `0644`). Only the file's owner or root may do this.
- **Returns:** `0` on success, `-EFAULT`/`-EINVAL` for a bad path, `-ENOENT` if not found, `-EPERM` if the caller is neither the owner nor root.
- **Implementation:** `sys_chmod()`, writes the inode's `perm` field back via `lufirafs_write_inode()` + `lufirafs_sync()`.

**SYS_CHOWN (20)**
- **Signature:** `long sys_chown(const char *path, int uid, int gid)`
- **Description:** Changes a file's owner and group. Root only — no POSIX "owner may change to their own group" carve-out.
- **Returns:** `0` on success, `-EPERM` if the caller isn't root, `-ENOENT` if not found.
- **Implementation:** `sys_chown()`.

**SYS_GETUID (21) / SYS_GETGID (22)**
- **Signature:** `long sys_getuid(void)` / `long sys_getgid(void)`
- **Description:** Return the calling process's identity. No arguments.
- **Returns:** `current_process->uid` / `->gid`.

**SYS_MKDIR (23)**
- **Signature:** `long sys_mkdir(const char *path, int mode)`
- **Description:** Creates a directory, resolved relative to `cwd_inode`. `mode` is accepted (POSIX signature compatibility) but currently ignored — new directories always get LufiraFS's fixed default permission. Requires write+exec on the parent directory.
- **Returns:** `0` on success, `-EACCES` on a parent permission failure, `-ENOENT` if the parent doesn't resolve, `-1` on other VFS failures.
- **Implementation:** `lufirafs_check_access()` on the parent, then `vfs_mkdir_at()`.

**SYS_RMDIR (24) / SYS_UNLINK (25)**
- **Signature:** `long sys_rmdir(const char *path)` / `long sys_unlink(const char *path)`
- **Description:** Remove a directory / a file, resolved relative to `cwd_inode`. LufiraFS doesn't distinguish the two at the `lufirafs_unlink()` level, so both syscalls share one internal helper (`sys_remove()`) — neither checks that `path` is actually the right kind of entry. Requires write+exec on the parent directory.
- **Returns:** `0` on success, `-EACCES` on a parent permission failure, `-ENOENT` if the parent doesn't resolve, `-1` on other VFS failures.
- **Implementation:** `sys_remove()` → `vfs_rmdir_at()` / `vfs_unlink_at()`.

**SYS_READDIR (26)**
- **Signature:** `long sys_readdir(int fd, struct lufira_dirent *out)`
- **Description:** Reads the next entry from a directory previously opened with `SYS_OPEN`. No separate permission check — read access was already verified when the directory was opened (which is also, incidentally, why directory listing is now permission-checked at all — see [`15_users_permissions.md`](15_users_permissions.md#enforcement-coverage)).
- **Returns:** `1` with `*out` filled in, `0` at end of directory, `-EFAULT` for a bad `out` pointer, `-1` on other errors.
- **Implementation:** Validates `out` (`sizeof(vfs_dirent_t)`, writable), then calls `vfs_readdir()`.

### System Information

**SYS_GETTICK (5)**
- **Signature:** `long sys_gettick(void)`
- **Description:** Returns the number of timer ticks since boot.
- **Returns:** Timer tick count.
- **Implementation:** Returns `pit_get_ticks()`.

**SYS_STATFS (27) / SYS_MEMINFO (28) / SYS_CPULOAD (29)**
- **Signature:** `long sys_statfs(struct lufira_statfs *out)` / `long sys_meminfo(struct lufira_meminfo *out)` / `long sys_cpuload(struct lufira_cpuload *out)`
- **Description:** Copy a fixed-size snapshot struct (block/inode counts; physical-page and kernel-heap totals; raw PIT tick counters) into `out`. Mirror what the old kernel-native `df`/`free`/`cpuload` commands used to print directly — now these are userspace packages (`lufira-packages/user/{df,free,cpuload}.c`) that call these syscalls instead. `SYS_CPULOAD` returns raw tick counters; the caller samples twice with a `sleep()` in between and computes the busy percentage itself.
- **Returns:** `0` on success, `-EFAULT` for a bad `out` pointer, `-EINVAL` if an internal size check fails.
- **Implementation:** Validates `out`, then fills it from `lufirafs.sb`/`pmm_get_*()`/`heap_get_stats()`/`pit_get_ticks()` respectively.

**SYS_PSLIST (30)**
- **Signature:** `long sys_pslist(struct lufira_ps_entry *out, unsigned long max_count)`
- **Description:** Writes up to `max_count` entries (`pid`/`ppid`/`name`/`state`/`uid`/`cpu_ticks`) from the live process table into `out`. Backs the `ps` package.
- **Returns:** Number of entries written, `-EFAULT` for a bad `out` pointer.
- **Implementation:** Iterates `process_list`, copying into `lufira_ps_entry_t` records.

---

### Privileged System Control (31–40)

These exist specifically to let functionality that used to be kernel-native shell commands (`kernel/shell/commands/{system,users}.c`, now dead code — see [`14_shell_commands.md`](14_shell_commands.md)) move into ordinary userspace packages, without giving every package direct access to kernel internals like `users_add()` or the reset-controller I/O port.

**SYS_SET_FOREGROUND (31) / SYS_GET_FOREGROUND (49)**
- **Signature:** `long sys_set_foreground(long pid)` / `long sys_get_foreground(void)`
- **Description:** `SYS_SET_FOREGROUND` marks `pid` (0 to clear) as the "foreground" process — only a process's own direct child, checked via `ppid` (and, since the v0.8 bridge, any descendant in the chain, not just a direct child). When the keyboard driver sees Ctrl+C, it signals `SIGINT` to whichever PID is currently foreground. This is how `shell.elf` implements Ctrl+C on a `run`/`runbg`'d child: `fork()` → `SYS_SET_FOREGROUND(child_pid)` → `SYS_WAIT(child_pid)`. `SYS_GET_FOREGROUND` just reads the value back (0 if unset).
- **Returns:** `0` / the current foreground PID.
- **Implementation:** Thin wrappers over `process_set_foreground()`/`process_get_foreground()` (`process.h`/`.c`).

**SYS_SU (32)**
- **Signature:** `long sys_su(const char *username, const char *password)`
- **Description:** Checks `password` against `users_check_password()` **inside the kernel** — userspace is never trusted to call this only "after" its own check, since that would let root-equivalent code be forged by any process. On success, mutates the *calling* process's own `uid`/`gid` in place (there is no separate session/login object — the shell process **is** the identity). Root may switch to anyone with no password. Does not touch `cwd` — the caller (`shell.elf`'s `su` builtin) does its own `cd` to the target's home directory afterward.
- **Returns:** `0` on success, `-1` on authentication failure.
- **Implementation:** `users_check_password()` then mutates `current_process->uid`/`gid`.

**SYS_MOUNT (33) / SYS_UNMOUNT (34)**
- **Signature:** `long sys_mount(const char *prefix, long usb_index)` / `long sys_unmount(const char *prefix)`
- **Description:** Mounts the given USB Mass Storage device's FAT filesystem under a VFS path prefix (e.g. `/mnt/usb0`) — ordinary `open`/`read`/`write`/`mkdir`/`unlink`/`readdir` start seeing that device's root directory directly under the prefix, with no separate mount-specific read/write syscalls (see [`08_filesystem.md`](08_filesystem.md#fat-and-usb-mass-storage-mounting)). Writes sync back to the USB device in real time, after every write — not just on unmount. Only the mount's root level is supported (no FAT subdirectory traversal within the mounted device).
- **Returns:** `SYS_MOUNT`: `>=0` (mount slot) on success; negative error codes for bad prefix (`-1`), prefix already mounted (`-2`), no free mount slots — max 2 (`-3`), no such USB device (`-4`), unsupported block size (`-5`), device too large (`-6`), out of memory (`-7`), read failure (`-8`), not FAT (`-9`). `SYS_UNMOUNT`: `0` on success, `-1` if no such mount.
- **Implementation:** `vfs_fat_mount()`/`vfs_fat_unmount()` (`fs/fat/fat_mount.c`).

**SYS_REBOOT (35) / SYS_SHUTDOWN (36)**
- **Signature:** `long sys_reboot(void)` / `long sys_shutdown(void)`
- **Description:** Root-only, irreversible for the whole system. Syncs LufiraFS (`lufirafs_flush()`) before actually resetting/shutting down.
- **Returns:** Does not return on success; `-EPERM` if the caller isn't root.
- **Implementation:** Direct port I/O for reset/ACPI shutdown, after `lufirafs_flush()`.

**SYS_DEVMODE (37)**
- **Signature:** `long sys_devmode(long mode)`
- **Description:** `mode == 0` reads the current state (returns 0/1), `1` enables, `2` disables. Thin wrapper over `devmode_set()`/`devmode_is_enabled()` (see [`04_logging.md`](04_logging.md)).
- **Returns:** Current/new state, or `0`/`1` as described.
- **Implementation:** `devmode_set()`/`devmode_is_enabled()`.

**SYS_USERADD (38)**
- **Signature:** `long sys_useradd(const char *username, const char *password, const char *group)`
- **Description:** Root-only. `group == NULL` creates a new group matching the username (real Unix `useradd` default behavior). Thin wrapper over `users_add()`/`groups_add()`, plus creation of a private `/home/<user>` home directory.
- **Returns:** `0` on success, `-EPERM` if not root, `-1` on other failures (duplicate name, table full).
- **Implementation:** `users_add()`/`groups_add()` (`system/users/users.c`) plus home-directory creation.

**SYS_GROUPADD (39)**
- **Signature:** `long sys_groupadd(const char *groupname)`
- **Description:** Root-only. Thin wrapper over `groups_add()`.
- **Returns:** `0` on success, `-EPERM` if not root, `-1` if the name already exists or the table is full.

**SYS_PASSWD (40)**
- **Signature:** `long sys_passwd(const char *username, const char *new_password)`
- **Description:** `username == NULL` changes the *caller's own* password (no extra check — the caller is already authenticated as itself). A non-NULL `username` resets any user's password and is root-only.
- **Returns:** `0` on success, `-EPERM` if a non-root caller names someone else, `-1` if the user doesn't exist.

---

### USB Mass Storage (41–44)

Direct raw-block access to USB Mass Storage devices, independent of the FAT mount layer above. Backs the `usbinfo`/`usbread`/`usbwrite` packages.

**SYS_USB_COUNT (41)**
- **Signature:** `long sys_usb_count(void)`
- **Description:** Number of detected USB Mass Storage devices.
- **Returns:** Device count.
- **Implementation:** `xhci_msd_device_count()`.

**SYS_USB_INFO (42)**
- **Signature:** `long sys_usb_info(long index, struct lufira_usb_info *out)`
- **Description:** Geometry (`max_lba`, `block_size`) of device `index`.
- **Returns:** `0` on success, `-1` if no such device, `-EFAULT` for a bad `out` pointer.

**SYS_USB_READ (43) / SYS_USB_WRITE (44)**
- **Signature:** `long sys_usb_read(long index, unsigned long lba, void *buf, unsigned long buf_size)` / `long sys_usb_write(long index, unsigned long lba, const void *buf, unsigned long buf_size)`
- **Description:** Read/write exactly one block at LBA `lba`. `buf_size` must be at least the device's real block size (same pattern as `SYS_MEMINFO`/`SYS_STATFS` with their struct-size checks) or the call fails with `-EINVAL`. Writing is root-only — direct LBA writes are irreversible and can corrupt a filesystem mounted on the same device via `SYS_MOUNT`.
- **Returns:** `0`/bytes on success; `-EINVAL` for a too-small buffer, `-1` for other device errors, `-EPERM` for `SYS_USB_WRITE` from a non-root caller.

---

### Polling, Signals, Alarms (45–49)

Added in the "v0.8 bridge" stage specifically so the window manager (and any ordinary program) can wait on more than one thing — input plus several client connections — without a dedicated process per connection, and so a long-running GUI program can get a timer tick without resorting to a fully blocking `sleep()`.

**SYS_POLL (45)**
- **Signature:** `long sys_poll(struct lufira_pollfd *fds, unsigned long nfds, long timeout_ms)`
- **Description:** `fds` is an array of `{fd, events, revents}`. `timeout_ms == 0` polls once and returns immediately; `>0` waits up to that many milliseconds; `<0` waits indefinitely. Implemented as a step-wise poll at one PIT tick's granularity (~10 ms, via `process_sleep()`) rather than a true per-fd wakeup — registering a single process as a waiter on several independent wait objects at once (pipes, console input, …) simultaneously would need every one of those objects to support multi-registration and clean deregistration on first match, which the existing single-waiter-slot wait primitives don't do. At the ~10 ms granularity this matters for, the difference is not observable.
- **Returns:** Number of fds with nonzero `revents`, `0` on timeout, `-1`/negative errno otherwise.
- **Implementation:** `vfs_poll_check()` per fd, looped with `process_sleep(1 tick)` between passes.

**SYS_SIGACTION (46) / SYS_SIGRETURN (47)**
- **Signature:** `long sys_sigaction(int sig, void (*handler)(int))` / `void sys_sigreturn(void)` — noreturn
- **Description:** Installs a handler for `SIGINT` or `SIGTERM` only (`handler == 0` restores the default action) — `SIGKILL`/`SIGSTOP`/`SIGCONT` can't be caught, matching real POSIX, and any other signal number is rejected outright. `SYS_SIGRETURN` is **not** a normal C `return` from the handler — it's the mandatory way to end one, restoring the exact process state (registers, flags) that existed the instant the signal interrupted it. It's special-cased in `syscall_handler()` (needs the raw frame pointer, like `SYS_FORK`) rather than going through `syscall_table[]`.
- **Returns:** `SYS_SIGACTION`: `0` on success, `-EINVAL` for a disallowed signal number. `SYS_SIGRETURN`: never returns to its caller in the normal sense.
- **Implementation:** `process_sigaction()`/`process_sigreturn()` (`process.c`).

**SYS_ALARM (48)**
- **Signature:** `long sys_alarm(unsigned long milliseconds)`
- **Description:** Arms a one-shot `SIGALRM`, delivered exactly once after `milliseconds` (`0` cancels any pending alarm) — real POSIX `alarm()` semantics, not a repeating `setitimer()`. A caller wanting a periodic tick re-arms it from inside its own handler.
- **Returns:** `0`.
- **Implementation:** `process_set_alarm()`.

---

### GUI / Window Manager (50–62)

Full architectural detail — the mailbox IPC primitive underneath, the client/WM RPC protocol, and a pointer to the actual window-manager implementation — is in [`18_gui_wm.md`](18_gui_wm.md). This section only documents each syscall's own contract. As of v0.8, the window manager is an ordinary userspace process (`lufira-packages/apps/wm.c`), not kernel code — every `SYS_WIN_*` call here is a thin RPC wrapper: pack a request, `mailbox_send()` it to the registered WM's PID, block for the reply. The syscall *signatures* are unchanged from the WM's original (now-removed) kernel-resident implementation, so no client code (`libc`'s `gui_widgets.c`, any GUI app) had to change across that move.

**SYS_WIN_CREATE (50)**
- **Signature:** `long sys_win_create(int x, int y, unsigned int w, unsigned int h, const char *title)`
- **Description:** Creates a `w`×`h` window (client area only, no border/titlebar) at `(x, y)` and focuses it. Fails if no WM is registered (`SYS_WM_REGISTER`) — i.e. the GUI subsystem isn't running at all. `title` may be `NULL`.
- **Returns:** Window id (`>=0`), or `-1`.

**SYS_WIN_DESTROY (51)**
- **Signature:** `long sys_win_destroy(int window_id)`
- **Description:** Closes a window (only its owner may do this — the WM checks). When the last window in the system closes, GUI mode turns itself off automatically and the screen reverts to the text console.
- **Returns:** `0`/`-1`.

**SYS_WIN_FILL (52) / SYS_WIN_DRAW_RECT (53) / SYS_WIN_DRAW_TEXT (54)**
- **Signature:** `long sys_win_fill(int window_id, unsigned int color)` / `long sys_win_draw_rect(int window_id, int x, int y, unsigned int w, unsigned int h, unsigned int color)` / `long sys_win_draw_text(int window_id, int x, int y, const char *text, unsigned int color)`
- **Description:** Drawing primitives in client-area-relative coordinates (`(0,0)` = the window's own top-left, not the screen's). No icons, only rectangles and bitmap text (the same 8x8 font the text console uses). `text` is truncated to 63 bytes in transit (`wm_request_t.str`) — long enough for every real caller today.
- **Returns:** `0`/`-1`.

**SYS_WIN_POLL_EVENT (55)**
- **Signature:** `long sys_win_poll_event(int window_id, struct lufira_gui_event *out)`
- **Description:** Checks for a pending input event on this window. Non-blocking from the application's point of view (the RPC to the WM itself is still a short synchronous call).
- **Returns:** `1` with `*out` filled in, `0` if the queue is empty.

**SYS_WIN_MOVE (56)**
- **Signature:** `long sys_win_move(int window_id, int x, int y)`
- **Description:** Programmatic window move, in addition to dragging the titlebar with the mouse (which the WM itself handles).
- **Returns:** `0`/`-1`.

**SYS_WM_REGISTER (59)**
- **Signature:** `long sys_wm_register(void)`
- **Description:** Makes the caller the system's **one** window server. Every subsequent `SYS_WIN_*` call and all raw keyboard/mouse input get routed to this PID's mailbox from then on, and only this PID gets access to `SYS_FB_PRESENT`.
- **Returns:** `0` on success, `-1` if someone else is already registered.

**SYS_FB_INFO (60)**
- **Signature:** `long sys_fb_info(struct lufira_fb_info *out)`
- **Description:** Open to any process — just reads screen dimensions and pixel format (`{width, height, pixel_format}`; `pixel_format` is 0/1, matching `console.c`'s internal RGB/BGR distinction, which the WM has to replicate itself since it no longer runs inside the kernel). Needed by the WM at startup to size its compositor buffer.
- **Returns:** `0` on success, `-EFAULT` for a bad pointer.

**SYS_FB_PRESENT (61)**
- **Signature:** `long sys_fb_present(const uint32_t *buf, unsigned int w, unsigned int h)` (full-frame) / `long sys_fb_present_rect(const uint32_t *buf, unsigned int w, unsigned int h, int dx, int dy, unsigned int dw, unsigned int dh)` (dirty-rectangle variant)
- **Description:** `buf` must always be a complete `w*h*4`-byte frame matching the current screen resolution, but only the `(dx, dy, dw, dh)` sub-rectangle of it is actually copied into the real framebuffer — a dirty-rectangle optimization added after a reported bug where every mouse movement re-presented the *entire* frame (expensive, since the real framebuffer is often slow MMIO/VRAM) just to move a small cursor sprite, causing visible lag. The full-frame form (`sys_fb_present()`) is exactly the dirty-rect form with the rectangle set to "the whole frame" and remains valid for any old call site. Restricted to the registered WM PID — anyone else gets `-EPERM`.
- **Returns:** `0` on success, `-EPERM` if not the registered WM, `-EINVAL` for a buffer-size mismatch, `-EFAULT` for a bad pointer.

**SYS_FB_FONT (62)**
- **Signature:** `long sys_fb_font(void *out, unsigned long max_bytes)`
- **Description:** Open to any process — copies the kernel's built-in 8x8 bitmap font (otherwise a `static` the console driver never exposes) into `out`, up to `max_bytes`. Needed by the WM to draw its own titlebar/close-button text without a kernel round-trip per character.
- **Returns:** The font's real size in bytes (may be larger than `max_bytes`, in which case only the first `max_bytes` were copied — the caller re-calls with a bigger buffer if it needs the rest).

---

### Generic IPC (57–59)

See also [`18_gui_wm.md`](18_gui_wm.md) for the full mailbox design — this is a general-purpose primitive, not GUI-specific; the WM protocol is just its first (and so far only) consumer.

**SYS_IPC_SEND (57)**
- **Signature:** `long sys_ipc_send(long dest_pid, const void *msg, unsigned long len)`
- **Description:** Copies `len` bytes (`<= MAX_IPC_MSG_PAYLOAD`, 96) from `msg` into `dest_pid`'s mailbox. Works between **any** two processes, not just parent/child — unlike pipes (inherited fds only) or `shm.c` (fork-time `MAP_SHARED` only), this is the first way two unrelated processes can talk directly.
- **Returns:** `0` on success, `-EINVAL` (no such pid, or `len` too large), `-EAGAIN` (destination mailbox full — 32-deep queue, not expected to actually fill up under current usage).

**SYS_IPC_RECV (58)**
- **Signature:** `long sys_ipc_recv(struct lufira_ipc_msg *out, int timeout_ms)`
- **Description:** Dequeues the next message from the *caller's own* mailbox into `out` (`sender_pid` + `len` + payload). `timeout_ms < 0` blocks indefinitely at zero CPU cost while empty; `0` returns immediately if empty; `>0` waits up to that long (same tick-granularity poll as `SYS_POLL`).
- **Returns:** `1` if a message was received, `-1` if none arrived before the timeout.

---

### Miscellaneous (65–66)

**SYS_CONSOLE_INJECT (63) / SYS_CONSOLE_REDRAW (64) — retired**

These existed only in the GUI subsystem's first (kernel-resident) cut, when the desktop was drawn only while at least one window was open: the WM injected keystrokes back into the text console while 0 windows existed, and asked for a console redraw when the last window closed. Since the desktop (background, icons, taskbar — `lufira-packages/apps/wm.c`) is now drawn continuously from the moment `SYS_WM_REGISTER` succeeds, neither case arises anymore — the way back to the text console is the taskbar's "Exit" button, which just calls the WM's own `sys_exit()`; `process_exit()` already calls `console_redraw_from_history()` directly, with no syscall needed. The numbers are deliberately left unused rather than reassigned to anything else.

**SYS_DUP2 (65)**
- **Signature:** `long sys_dup2(int oldfd, int newfd)`
- **Description:** Thin wrapper over the pre-existing `vfs_dup2()` (the same mechanism `fork()` already uses internally to duplicate its fd table) — now exposed directly to userspace. Needed by the GUI terminal app (`lufira-packages/apps/terminal.c`): before `exec()`ing a child `/bin/shell.elf`, it redirects the child's fd 0/1/2 onto the ends of two `pipe()`s, turning a GUI window into a real terminal wrapped around a real `shell.elf`, rather than a separate reimplementation of a command parser.
- **Returns:** `newfd` on success, `-1` for a bad `oldfd`/`newfd` (same range checks as `vfs_dup2()`).

**SYS_NET_FETCH (66)**
- **Signature:** `long sys_net_fetch(const char *url, void *out_buf, unsigned long out_cap, int *status_out)`
- **Description:** The package manager's (`dlpg sync`/`upgrade`, see [`17_package_manager.md`](17_package_manager.md)) only network syscall, and currently its only caller. Not a raw socket API (no separate `SEND`/`RECV`/`CONNECT`) — one call does URL parsing (scheme/host/port/path), DNS resolution if the host isn't a literal IP, TCP, TLS 1.2 for `https://`, and full HTTP response parsing (including chunked transfer-encoding) via `http_fetch()` (`kernel/net/http_client.c`). Full protocol detail is in [`16_networking.md`](16_networking.md).

  `out_buf`/`out_cap` receive the response **body only** (no status line/headers). `status_out` (may be `NULL`) receives the HTTP status code if parsing got that far, regardless of whether it's 2xx — a 404 is a successful fetch of a 404 body, not a `sys_net_fetch()` error.

  **Security note on `https://` (stated plainly, not buried):** the TLS client verifies the ServerKeyExchange signature against the key in the certificate the server itself presented — this defeats a naive/passive on-path attacker with no valid key for any certificate at all. It does **not** validate the certificate chain against any trusted root CA, because **there is no bundled set of trusted root certificates in this OS at all.** An attacker able to intercept the connection and present their own self-signed certificate (with a key they control) passes this check. This is a deliberate, documented scope limitation — a full chain validation would need an embedded root-CA trust store, X.509 extension parsing, and hostname-to-certificate matching — not an oversight; the exact same wording appears in the kernel's own `tls.c` and `syscall.h` comments. Do not rely on this for anything where a real MITM is a credible threat.
- **Returns:** `>=0` — body length in `out_buf` (`<= out_cap`) on success. Negative error codes distinguish DNS/TCP/TLS/HTTP-parse/`ENOSPC`/no-network-device failures individually (see `http_client.h`'s `HTTP_FETCH_E*` constants, mirrored as `NET_FETCH_E*` in `libc/include/lufira/syscall.h`) rather than a single generic `-1`.
- **Implementation:** `http_fetch()` (`kernel/net/http_client.c`), called with the caller's validated `url`/`out_buf`.

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
| `EPERM` | 1 | Operation not permitted (e.g. `chmod`/`chown` by a non-owner/non-root, `reboot`/`shutdown`/`SYS_USB_WRITE` by a non-root caller). |
| `ENOENT` | 2 | No such file or directory. |
| `EAGAIN` | 11 | Would block right now (e.g. `SYS_IPC_SEND` into a full mailbox). |
| `EACCES` | 13 | Permission denied (owner/group/other bits). |
| `EFAULT` | 14 | Bad user pointer (unmapped, or not accessible from ring 3). |
| `ENOTDIR` | 20 | Not a directory. |
| `EINVAL` | 22 | Invalid argument (e.g. an empty path string, a disallowed signal number, a too-small buffer for a fixed-size struct). |
| `ERANGE` | 34 | Result doesn't fit in the caller's buffer (`SYS_GETCWD`). |

Not every syscall failure carries one of these — some VFS-level failures (bad file descriptor, an underlying LufiraFS error with no distinct code of its own) still collapse to a bare `-1`, since the VFS layer itself has no error-code system yet. `SYS_NET_FETCH` is the one syscall with its own independent, wider error-code space (`NET_FETCH_E*`, see [Miscellaneous](#miscellaneous-6566)) rather than reusing this table at all.

---

## Dependencies

| Component | Depends On | Purpose |
|-----------|------------|---------|
| System Calls | GDT, TSS | User/kernel mode transitions. |
| System Calls | Process Manager | Process operations, per-process `uid`/`gid`/cwd/mmap-region/foreground/mailbox state. |
| System Calls | Paging | User-pointer validation (`is_user_range_valid()`). |
| System Calls | VFS / LufiraFS | File, directory, and permission operations. |
| System Calls | PIT | Timer-related operations, and the tick-granularity poll loops behind `SYS_POLL`/`SYS_IPC_RECV`. |
| System Calls | `users.c` | `SYS_SU`/`SYS_USERADD`/`SYS_GROUPADD`/`SYS_PASSWD`. |
| System Calls | `fs/fat/fat_mount.c` | `SYS_MOUNT`/`SYS_UNMOUNT`. |
| System Calls | `drivers/usb/xhci.c` | `SYS_USB_*`. |
| System Calls | `system/ipc/mailbox.c` | `SYS_IPC_SEND`/`RECV` directly, and every `SYS_WIN_*`/`SYS_WM_REGISTER`/`SYS_FB_*` RPC indirectly (see [`18_gui_wm.md`](18_gui_wm.md)). |
| System Calls | `drivers/console/console.c` | `SYS_FB_*`'s framebuffer geometry/font/present path. |
| System Calls | `kernel/net/http_client.c` | `SYS_NET_FETCH`. |
| System Calls | Console | (For debugging only). |

---

## Conclusion

All 65 defined system calls are implemented, with user-pointer validation and errno-style error codes covering process control (`fork`/`exec` with real `argv`/`envp`/`wait`/signals/pipes), anonymous memory mapping, file and directory I/O, identity/permission management, privileged system control, USB Mass Storage access, GUI/window-manager RPC, generic inter-process messaging, and a one-call network fetch over DNS/TCP/TLS/HTTP. Most of the growth since v0.6.0's 27 syscalls exists for one reason: moving functionality (the shell, `su`/`mount`/`reboot`, the window manager) out of the kernel binary and into ordinary userspace processes needed a controlled interface to replace the direct kernel-internal function calls those features used to make. The use of the `syscall` instruction ensures fast transitions, and the calling convention follows the x86-64 ABI for compatibility.

For more details, refer to the source code in `kernel/system/syscall/`.

---

**Document Version:** 3.0
**Last Updated:** October 2026
**Project:** LufiraOS
