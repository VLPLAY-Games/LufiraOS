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

- Perform file operations (open, close, read, write, seek).
- Manage processes (create, terminate, sleep, kill).
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
| 11 | `SYS_EXEC` | Execute a program | Stub |
| 12 | `SYS_FORK` | Create a child process | Stub |
| 13 | `SYS_WAIT` | Wait for a child process | Stub |
| 14 | `SYS_GETCWD` | Get current working directory | Stub |
| 15 | `SYS_CHDIR` | Change current directory | Stub |
| 16 | `SYS_SLEEP` | Sleep for milliseconds | Implemented |
| 17 | `SYS_KILL` | Terminate a process | Implemented |

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

**SYS_KILL (17)**
- **Signature:** `int kill(pid_t pid)`
- **Description:** Terminates a process by PID.
- **Returns:** `0` on success, `-1` on error.
- **Implementation:** Calls `process_kill(pid)`.

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
| `SYS_EXEC` | Replaces the current process with a new program. |
| `SYS_FORK` | Creates a child process. |
| `SYS_WAIT` | Waits for a child process to terminate. |
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
| `SYS_KILL` | Process not found. |
| `SYS_MMAP` | Unsupported (always returns `-1`). |
| `SYS_EXEC` | Unsupported (always returns `-1`). |
| `SYS_FORK` | Unsupported (always returns `-1`). |

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

The system call interface provides a clean and efficient mechanism for user-mode programs to request kernel services. With 17 implemented system calls, it covers the essential functionality needed for basic user programs. The use of the `syscall` instruction ensures fast transitions, and the calling convention follows the x86-64 ABI for compatibility.

For more details, refer to the source code in `system/syscall/`.

---

**Document Version:** 1.0  
**Last Updated:** September 2026  
**Project:** LufiraOS