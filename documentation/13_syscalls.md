# System Calls

This document describes the system call interface of LufiraOS. System calls provide a controlled mechanism for user-mode programs to request services from the kernel.

---

## Table of Contents

1. [Overview](#overview)
2. [System Call Mechanism](#system-call-mechanism)
   - [MSR Configuration](#msr-configuration)
   - [System Call Entry Stub](#system-call-entry-stub)
   - [System Call Handler](#system-call-handler)
3. [System Call Table](#system-call-table)
4. [System Call Descriptions](#system-call-descriptions)
   - [File Operations](#file-operations)
   - [Process Operations](#process-operations)
   - [System Information](#system-information)
   - [Stubs for Future Implementation](#stubs-for-future-implementation)
5. [Calling Convention](#calling-convention)
6. [Error Handling](#error-handling)
7. [Dependencies](#dependencies)
8. [Future Extensions](#future-extensions)

---

## Overview

System calls provide the interface between user-mode programs and the kernel. They allow user programs to:

- Perform file operations (open, close, read, write, seek, pipe).
- Manage processes (fork, exec, wait, kill/signals, sleep).
- Query system information (PID, timer ticks).
- Control system behaviour (yield CPU, exit).

**Design Philosophy:**
- **Efficiency** – uses the `syscall` instruction for fast transitions.
- **Simplicity** – the calling convention matches the x86-64 ABI.
- **Safety** – arguments are validated before use.

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
2. Looks up the function pointer in the system call table.
3. Calls the function with the provided arguments.
4. Returns the result to the user program.

**Prototype:** `uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)`

---

## System Call Table

The system call table is an array of function pointers indexed by system call number. Unimplemented entries return `-1`.

| Number | Name | Description | Status |
|--------|------|-------------|--------|
| 0 | `SYS_WRITE` | Write to a file descriptor | Implemented |
| 1 | `SYS_READ` | Read from a file descriptor | Implemented |
| 2 | `SYS_EXIT` | Terminate the current process | Implemented |
| 3 | `SYS_GETPID` | Get the current process ID | Implemented |
| 4 | `SYS_YIELD` | Yield the CPU | Implemented |
| 5 | `SYS_GETTICK` | Get timer ticks since boot | Implemented |
| 6 | `SYS_OPEN` | Open a file | Implemented |
| 7 | `SYS_CLOSE` | Close a file descriptor | Implemented |
| 8 | `SYS_SEEK` | Reposition file offset | Implemented |
| 9 | `SYS_MMAP` | Memory map a file or device | Stub |
| 10 | `SYS_MUNMAP` | Unmap memory | Stub |
| 11 | `SYS_EXEC` | Replace the current process image with a new program | Implemented (see caveat below) |
| 12 | `SYS_FORK` | Create a child process | Implemented (see caveat below) |
| 13 | `SYS_WAIT` | Wait for a child process | Implemented |
| 14 | `SYS_GETCWD` | Get current working directory | Stub |
| 15 | `SYS_CHDIR` | Change current directory | Stub |
| 16 | `SYS_SLEEP` | Sleep for milliseconds | Implemented |
| 17 | `SYS_KILL` | Send a signal to a process | Implemented |
| 18 | `SYS_PIPE` | Create an anonymous pipe | Implemented |

> **Caveat:** `SYS_FORK`/`SYS_EXEC` are implemented but not fully reliable — see [`12_elf_processes.md` § Known Issue](12_elf_processes.md#known-issue) and [README.md § Known Issues](../README.md#known-issues-and-limitations).

---

## System Call Descriptions

### File Operations

**SYS_WRITE (0)**
- **Signature:** `int write(int fd, const void *buf, size_t count)`
- **Description:** Writes up to `count` bytes from `buf` to the file descriptor `fd`.
- **Returns:** Number of bytes written, or `-1` on error.
- **Implementation:** Calls `vfs_write()`.

**SYS_READ (1)**
- **Signature:** `int read(int fd, void *buf, size_t count)`
- **Description:** Reads up to `count` bytes from `fd` into `buf`.
- **Returns:** Number of bytes read, or `-1` on error.
- **Implementation:** Calls `vfs_read()`.

**SYS_OPEN (6)**
- **Signature:** `int open(const char *path, int flags, int mode)`
- **Description:** Opens a file specified by `path`.
- **Returns:** File descriptor, or `-1` on error.
- **Implementation:** Calls `vfs_open()`.

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
- **Description:** Yields the CPU to another process.
- **Returns:** `0`.
- **Implementation:** Calls `schedule()`.

**SYS_SLEEP (16)**
- **Signature:** `unsigned int sleep(unsigned int milliseconds)`
- **Description:** Sleeps for `milliseconds`.
- **Returns:** `0` (always).
- **Implementation:** Calls `process_sleep(milliseconds)`.

**SYS_FORK (12)**
- **Signature:** `pid_t fork(void)`
- **Description:** Duplicates the calling process (address space, file descriptors). Handled specially in `syscall_handler()` rather than through the normal `syscall_table[]`, since it needs a pointer to the entire saved register frame, not just the usual five arguments.
- **Returns:** Child PID to the parent, `0` to the child, `(uint64_t)-1` on failure.
- **Implementation:** Calls `process_fork(frame_ptr)`. See [`12_elf_processes.md`](12_elf_processes.md#fork) for the address-space cloning details and its known reliability issue.

**SYS_EXEC (11)**
- **Signature:** `int exec(const char *filename, char **argv, char **envp)`
- **Description:** Replaces the calling process's image with a new ELF program, in place (same PID). `argv`/`envp` are accepted but not yet passed to the new program.
- **Returns:** Does not return on success; `-1` on error (e.g. file not found).
- **Implementation:** Calls `do_exec()` → `elf_exec_replace()`.

**SYS_WAIT (13)**
- **Signature:** `pid_t wait(pid_t pid, int *status)`
- **Description:** Blocks until the given child (`pid == 0` = any child) terminates.
- **Returns:** The reaped child's PID, or `-1` if the caller has no such child.
- **Implementation:** Calls `process_wait(pid, &status)`.

**SYS_KILL (17)**
- **Signature:** `int kill(pid_t pid, int sig)`
- **Description:** Sends a signal to a process (`sig == 0` defaults to `SIGTERM`). Only default actions are implemented — no user-space signal handlers.
- **Returns:** `0` on success, `-1` if the process was not found.
- **Implementation:** Calls `process_signal(pid, sig)`. See [`12_elf_processes.md` § Signals and kill](12_elf_processes.md#signals-and-kill).

**SYS_PIPE (18)**
- **Signature:** `int pipe(int fds[2])`
- **Description:** Creates an anonymous pipe; `fds[0]` is the read end, `fds[1]` the write end, both in the calling process's own file descriptor table.
- **Returns:** `0` on success, `-1` on error.
- **Implementation:** Calls `vfs_pipe(fds)`.

### System Information

**SYS_GETTICK (5)**
- **Signature:** `uint64_t gettick(void)`
- **Description:** Returns the number of timer ticks since boot.
- **Returns:** Timer tick count.
- **Implementation:** Returns `pit_get_ticks()`.

### Stubs for Future Implementation

| System Call | Description |
|-------------|-------------|
| `SYS_MMAP` | Memory maps a file or device. |
| `SYS_MUNMAP` | Unmaps a memory mapping. |
| `SYS_GETCWD` | Gets the current working directory. |
| `SYS_CHDIR` | Changes the current working directory. |

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
| `R9` | Argument 6 |
| `RCX` | Clobbered (saved RIP) |
| `R11` | Clobbered (saved RFLAGS) |

**Return Value:**
- `RAX` contains the return value.
- A negative value indicates an error.

## Error Handling

System calls return `-1` on error (or an appropriate negative value). The error code is not exposed to user programs; the kernel only indicates success or failure.

**Error Conditions:**

| System Call | Error Condition |
|-------------|-----------------|
| `SYS_OPEN` | File not found or invalid path. |
| `SYS_READ`/`SYS_WRITE` | Invalid file descriptor. |
| `SYS_CLOSE` | Invalid file descriptor. |
| `SYS_SEEK` | Invalid file descriptor or unsupported operation. |
| `SYS_KILL` | Process not found or unknown signal. |
| `SYS_WAIT` | No such child (including one already reaped). |
| `SYS_FORK` | Out of memory while cloning the address space. |
| `SYS_EXEC` | File not found, or not enough memory for the new address space. |
| `SYS_MMAP` / `SYS_MUNMAP` | Unsupported (stubs, always return `0`). |
| `SYS_GETCWD` / `SYS_CHDIR` | Unsupported (stubs). |

---

## Dependencies

| Component | Depends On | Purpose |
|-----------|------------|---------|
| System Calls | GDT, TSS | User/kernel mode transitions. |
| System Calls | Process Manager | Process operations. |
| System Calls | VFS | File operations. |
| System Calls | PIT | Timer-related operations. |
| System Calls | Console | (For debugging only). |

---

## Conclusion

The system call interface provides a clean and efficient mechanism for user-mode programs to request kernel services. With 15 of 19 defined system calls implemented (`mmap`, `munmap`, `getcwd`, `chdir` remain stubs), it covers process control (including `fork`/`exec`/`wait`/signals/pipes as of v0.3.0), file I/O, and basic system information. The use of the `syscall` instruction ensures fast transitions, and the calling convention follows the x86-64 ABI for compatibility.

For more details, refer to the source code in `system/syscall/`.

---

**Document Version:** 1.1  
**Last Updated:** September 2026  
**Project:** LufiraOS