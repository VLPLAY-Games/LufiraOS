# ELF Loader and Process Management

This document describes the ELF executable loader and the process management subsystem of LufiraOS, including process creation, scheduling, and context switching.

---

## Table of Contents

1. [Overview](#overview)
2. [ELF Loader](#elf-loader)
   - [ELF Header Validation](#elf-header-validation)
   - [Program Header Parsing](#program-header-parsing)
   - [Segment Loading](#segment-loading)
   - [Memory Mapping](#memory-mapping)
   - [Entry Point](#entry-point)
3. [Process Management](#process-management)
   - [Process Structure](#process-structure)
   - [Process States](#process-states)
   - [Process Creation](#process-creation)
   - [Process Termination](#process-termination)
   - [Process Reaping](#process-reaping)
4. [fork(), exec(), wait(), and Signals](#fork-exec-wait-and-signals)
   - [fork()](#fork)
   - [exec() (In-Place Replace)](#exec-in-place-replace)
   - [wait()](#wait)
   - [Signals and kill](#signals-and-kill)
   - [Pipes](#pipes)
   - [The Kernel-Space Physical Memory Map](#the-kernel-space-physical-memory-map)
   - [Known Issue](#known-issue)
5. [Scheduling](#scheduling)
   - [Scheduler Algorithm](#scheduler-algorithm)
   - [Context Switching](#context-switching)
   - [Idle Process](#idle-process)
6. [Dependencies](#dependencies)
7. [Future Extensions](#future-extensions)

---

## Overview

The ELF loader and process management subsystem provides:

- **ELF64 Loading** – loads 64-bit ELF executables (ET_EXEC and ET_DYN) into memory.
- **Process Creation** – allocates processes with their own address spaces and stacks.
- **Scheduling** – round-robin scheduling with cooperative multitasking.
- **Context Switching** – saves and restores CPU state when switching between processes.

**Design Philosophy:**
- **Simplicity** – the scheduler uses a simple circular linked list.
- **Modularity** – the ELF loader is separate from process management.
- **Performance** – context switching is fast and efficient.

---

## ELF Loader

The ELF loader reads 64-bit ELF files and loads them into a process's address space.

### ELF Header Validation

The ELF header is validated by `elf_validate()`:

| Field | Required Value | Description |
|-------|----------------|-------------|
| `magic` | `0x464C457F` | ELF magic number ("\x7FELF"). |
| `elf_class` | `ELFCLASS64` (2) | Must be a 64-bit executable. |
| `machine` | `EM_X86_64` (62) | Must target x86-64 architecture. |
| `type` | `ET_EXEC` (2) or `ET_DYN` (3) | Executable or position-independent. |
| `phnum` | > 0 | Must have at least one program header. |

### Program Header Parsing

The loader parses program headers of type `PT_LOAD`:

| Field | Description |
|-------|-------------|
| `type` | Must be `PT_LOAD` (1). |
| `offset` | Offset of the segment in the file. |
| `vaddr` | Virtual address where the segment should be loaded. |
| `filesz` | Size of the segment in the file. |
| `memsz` | Size of the segment in memory (may be larger than `filesz`). |
| `flags` | Segment permissions: `PF_R`, `PF_W`, `PF_X`. |

### Segment Loading

For each `PT_LOAD` segment, the loader:

1. **Aligns the start and end addresses** to page boundaries.
2. **Allocates physical pages** for each page in the segment.
3. **Maps the pages** into the process's address space (temporarily writable).
4. **Zeroes the segment** in the process's memory.
5. **Copies the file contents** to the process's memory.

**Temporary Writable Mapping:**
- All pages are mapped with `PAGE_WRITE` during loading.
- After copying, permissions are set to the segment's final flags.
- The NX (No Execute) bit is set if the segment is not executable.

### Memory Mapping

The loader uses the process's page table (PML4) for mapping:

1. **Create Page Tables** – traverse or create PML4, PDPT, PD, and PT entries.
2. **Map Pages** – set each page table entry with the physical address and flags.
3. **Handle Huge Pages** – if a huge page exists, split it into 4 KiB pages.

**Flags Used:**

| Flag | Purpose |
|------|---------|
| `PAGE_PRESENT` | Page is present in memory. |
| `PAGE_WRITE` | Page is writable (during loading, always set). |
| `PAGE_USER` | Page is accessible from user mode. |
| `PAGE_NX` | No Execute (if segment is not executable). |

### Entry Point

The loader returns the entry point from the ELF header (`header->entry`). This is the address where execution should begin.

---

## Process Management

### Process Structure

| Field | Type | Description |
|-------|------|-------------|
| `pid` | `uint32_t` | Process ID (unique identifier). |
| `ppid` | `uint32_t` | Parent PID (0 = no parent, e.g. before `fork()`/`wait()`). |
| `name` | `char[32]` | Process name. |
| `state` | `process_state_t` | Current state (READY, RUNNING, etc.). |
| `wakeup_tick` | `uint64_t` | Timer tick when a sleeping process should wake. |
| `exit_code` | `int` | Valid once `state == PROCESS_TERMINATED`. |
| `wait_target_pid` | `uint32_t` | 0 = not waiting; `WAIT_ANY_PID` = waiting on any child; otherwise a specific PID. |
| `context` | `process_context_t` | Saved CPU state for context switching. |
| `stack_base` | `uint64_t` | Base address of the user stack. |
| `stack_size` | `uint64_t` | Size of the user stack. |
| `ring0_stack` | `uint64_t` | Kernel stack pointer (for ring 0). |
| `ring0_stack_pages` | `uint64_t` | Pages allocated for the kernel stack. |
| `page_table` | `uint64_t` | Physical address of the PML4 table. |
| `fd_table` | `fd_table_t` | Per-process file descriptor table (see [`08_filesystem.md`](08_filesystem.md)). |
| `is_shell` | `int` | Marks the process currently "acting as" the interactive shell — see [Process Termination](#process-termination). |
| `next` | `struct process*` | Pointer to the next process in the circular list. |

### Process States

| State | Description |
|-------|-------------|
| `PROCESS_READY` | The process is ready to run but not currently executing. |
| `PROCESS_RUNNING` | The process is currently executing on the CPU. |
| `PROCESS_BLOCKED` | The process is waiting for an event (I/O, etc.). |
| `PROCESS_SLEEPING` | The process is sleeping until `wakeup_tick`. |
| `PROCESS_STOPPED` | Stopped by `SIGSTOP`, waiting for `SIGCONT`. |
| `PROCESS_TERMINATED` | The process has exited and is waiting to be reaped. |

### Process Creation

**Function:** `process_create(name, entry)`

1. Allocate and zero the process structure.
2. Assign a unique PID.
3. Create a new address space:
   - Allocate a PML4 page.
   - Copy kernel mappings from the kernel CR3.
4. Allocate and map the Ring 0 stack.
5. Allocate and map the user stack.
6. Initialise the process context:
   - Set `rip` to the entry point.
   - Set `rsp` to the top of the user stack.
   - Set `rflags` to `0x202` (interrupts enabled).
   - Set `cr3` to the process's PML4.
7. Add the process to the circular list.
8. Return the process pointer.

**Address Space Creation:**

The new PML4 is created by copying the kernel PML4:

1. Allocate a new PML4 page.
2. Copy all entries from the kernel PML4.
3. For user-space entries (indices 0-255), clear the `PAGE_USER` flag.
4. Return the physical address of the new PML4.

**Stack Allocation:**

| Stack | Address Range | Size | Purpose |
|-------|---------------|------|---------|
| User Stack | `USER_STACK_AREA_START + (pid * USER_STACK_SIZE)` | 16 KiB | User-mode stack. |
| Ring 0 Stack | `KERNEL_STACK_AREA_START + (pid * KERNEL_STACK_SIZE)` | 16 KiB | Kernel-mode stack. |

### Process Termination

**Function:** `process_exit()`

1. Mark the current process as `PROCESS_TERMINATED` and record `exit_code`.
2. If the exiting process was `is_shell` (i.e. it was — or, after `exec()`, used to be — the interactive shell), immediately create a fresh `"shell"` process and mark it `is_shell` before switching away, so the system is never left without an interactive prompt. `exec()` replaces a process's image in place without forking, so if the shell itself exits or is killed, no other process is left to fall back to.
3. Wake a parent blocked in `wait()` for this PID, if any.
4. Call `schedule()` (or switch directly to the waiting parent) to switch to another process.

**Note:** The process is not immediately freed. It is reaped by `process_reap()` or by the parent's `wait()` call.

### Process Reaping

**Function:** `process_reap()`

1. Iterate through the process list.
2. Find processes in the `PROCESS_TERMINATED` state.
3. Remove them from the list and free their memory.

**Reaping Strategy:**
- The kernel calls `process_reap()` periodically (e.g., in the idle loop).
- The idle process is never reaped.

---

## fork(), exec(), wait(), and Signals

v0.3.0 adds real process primitives on top of the basic process manager described above, exposed both as syscalls (see [`13_syscalls.md`](13_syscalls.md)) and shell commands (see [`14_shell_commands.md`](14_shell_commands.md)).

### fork()

**Function:** `process_fork(frame_ptr)`

`fork()` duplicates the calling process: a new `process_t` is created, and its address space is a deep, eager copy of the parent's —

1. The kernel half (PML4 indices 256–511) is set up the same way as for any new process (shared kernel mappings, not copied per-process).
2. The user half (indices 0–255) is walked recursively, and **every** occupied physical page of data is copied into a freshly allocated physical page for the child (`clone_address_space_deep()`). There is no copy-on-write — the paging code has no infrastructure for it yet, so every `fork()` pays the full cost of duplicating the parent's resident memory.
3. The child's saved register context is built from `frame_ptr`, the syscall entry stub's saved frame, so the child resumes at the exact same user-mode instruction as the parent, with `rax` (the fork return value) forced to 0.
4. The file descriptor table is duplicated with shared underlying `file_t`s (see [`08_filesystem.md`](08_filesystem.md)) and incremented reference counts, matching POSIX semantics for `fork()`+pipes.

Returns the child's PID to the parent, `0` to the child, or `(uint64_t)-1` on failure (in which case the partially-built child is discarded via `process_discard()`).

### exec() (In-Place Replace)

**Function:** `elf_exec_replace()` / `do_exec()`, backing both `SYS_EXEC` and the shell's `exec` command.

Unlike `run`/`runbg` (which create a brand-new process via `elf_exec()`/`elf_exec_background()`), `exec()` replaces the **current** process's image in place:

1. `process_prepare_exec()` builds a new address space and loads the new ELF into it, without touching the process's current (still-running) image.
2. Only once loading succeeds is the process switched over (`process_commit_exec()`): new `page_table`, new user stack, same PID, same position in the scheduler's list.
3. Execution jumps directly into the new program via `context_switch()` — this call never returns on success.

Because `run`/`exec`/`runbg` are usually invoked synchronously from inside `keyboard_irq_handler()` (see [Input Dispatcher](07_drivers.md#input-dispatcher)), both code paths manually re-arm the PIC's EOI on both controllers before jumping into the new process/image — otherwise, if the triggering keystroke happened to arrive over USB HID (polled from `timer_irq_handler()`), the timer's own IRQ0 would never be acknowledged and the entire scheduler would hang.

### wait()

**Function:** `process_wait(pid, status_out)`

Blocks the calling process until the given child (or, with `pid == 0`, any child) terminates, then reaps it and returns its PID and exit code. Returns `-1` immediately if the caller has no such child (including one already reaped).

### Signals and kill

LufiraOS implements only default signal actions — there is no `sigaction()`/user-space handler support. Delivery is synchronous, performed directly inside `process_signal(pid, sig)`:

| Signal | Effect |
|--------|--------|
| `SIGTERM` (15) / `SIGKILL` (9) | Terminates the process (`exit_code = 128 + sig`, matching real shells). |
| `SIGSTOP` (19) | Moves the process to `PROCESS_STOPPED`; if it is stopping itself, yields the CPU immediately. |
| `SIGCONT` (18) | Moves a `PROCESS_STOPPED` process back to `PROCESS_READY`. |

The shell's `kill [-SIGNAL] <pid>` command (default `-TERM`) and `Ctrl+C` (which sends the equivalent of `SIGTERM`/`SIGKILL` to whichever process is currently the shell's foreground job) both go through this same function.

### Pipes

Anonymous pipes (`vfs_pipe()`, `SYS_PIPE`) are implemented in the VFS layer, not here, but are what makes `fork()` useful for shell-style job control: a pipe's read and write ends are ordinary file descriptors backed by a shared ring buffer, and `fork()`'s file-descriptor duplication is what lets a parent and child (or two children) communicate through one. See [`08_filesystem.md`](08_filesystem.md).

### The Kernel-Space Physical Memory Map

A significant chunk of the paging/process/ELF code was reworked around a dedicated, never-hijacked kernel-space mapping of all physical memory (`phys_to_virt()`, PML4 index 256). Previously, code that needed to read/write a physical address as a raw pointer (page-table construction, `fork()`'s per-page copy, the initial stack frame written by `process_create()`) relied on PML4[0] staying identity-mapped (`virt == phys`) under every process's page tables. That held true only until a user ELF segment was mapped into the *same* physical address range its own low-memory identity map covered — at which point a "physical pointer" into that range could silently land on a live, often read-only, page of someone else's program code instead of the intended physical page. `phys_to_virt()` gives a mapping that is never touched by ELF loading or any per-process stack, so it is always safe regardless of what else is currently mapped.

### Known Issue

`fork()` and `exec()` are **not fully reliable**. Extensive debugging traced a forked child to crashing (typically a triple fault reported by QEMU as an invalid MMIO read at `0xFED40000`, an address never referenced by this kernel's own code) somewhere between `context_switch()` jumping into the child and the child executing its first instruction — after code bytes, stack contents, register values, and raw page-table entries were all verified to be byte-for-byte correct. The root cause remains open; treat `fork()`/`exec()` as experimental. See [README.md § Known Issues](../README.md#known-issues-and-limitations).

---

## Scheduling

### Scheduler Algorithm

The scheduler (`schedule()`) uses a simple round-robin algorithm:

1. Disable interrupts.
2. If `current_process` is `NULL`, set it to the first process in the list.
3. Find the next process in the circular list.
4. Skip processes that are not in the `READY` state.
5. If no process is ready, fall back to the idle process.
6. Switch to the next process.

**Fallback:** If no process is ready, the scheduler switches to the idle process.

### Context Switching

**Function:** `switch_to_process(next)`

1. Save the current process's state:
   - Mark it as `READY` if it was `RUNNING`.
2. Mark the next process as `RUNNING`.
3. Update the TSS (set `rsp0` to the next process's Ring 0 stack).
4. Call `context_switch(prev_context, next_context)`.

**Context Switch Assembly (`context_switch`):**

1. **Save Current Context:**
   - Save all general-purpose registers.
   - Save RSP, RIP, RFLAGS, and CR3.
2. **Switch Address Space:**
   - Load CR3 from the new context.
3. **Restore New Context:**
   - Restore all general-purpose registers.
   - Restore RSP, RIP, RFLAGS.
4. **Jump to New RIP.**

**Context Structure:**

| Offset | Field |
|--------|-------|
| 0 | RAX |
| 8 | RBX |
| 16 | RCX |
| 24 | RDX |
| 32 | RSI |
| 40 | RDI |
| 48 | RBP |
| 56 | R8 |
| 64 | R9 |
| 72 | R10 |
| 80 | R11 |
| 88 | R12 |
| 96 | R13 |
| 104 | R14 |
| 112 | R15 |
| 120 | RSP |
| 128 | RIP |
| 136 | RFLAGS |
| 144 | CR3 |

### Idle Process

The idle process (PID 0) runs when no other process is ready:

**Entry Point:** `idle_thread()`

1. Enable interrupts (`sti`).
2. Halt the CPU (`hlt`).
3. Call `schedule()`.

**Purpose:** The idle process uses minimal CPU time and allows the kernel to handle interrupts.

---

## Dependencies

| Component | Depends On | Purpose |
|-----------|------------|---------|
| ELF Loader | PMM, Paging, Heap | Memory allocation and mapping. |
| Process Manager | PMM, Paging, Heap, GDT, TSS | Process creation and context switching. |
| fork()/exec()/wait()/kill | Process Manager, VFS | Address-space cloning, fd-table duplication, pipes. |
| Scheduler | PIT, Process Manager | Timer interrupts for scheduling. |
| Context Switch | GDT, TSS | User/kernel mode transitions. |

---

## Conclusion

The ELF loader and process management subsystem provides the foundation for running user programs. The ELF loader supports 64-bit executables, and the process manager provides process creation, scheduling, and termination. The cooperative scheduler is simple but effective for a hobby OS.

For more details, refer to the source code in `system/elf/`, `system/process/`, and `system/cpu/` (for TSS and context switching).

---

**Document Version:** 1.1  
**Last Updated:** September 2026  
**Project:** LufiraOS