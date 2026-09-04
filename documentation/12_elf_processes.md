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
4. [Scheduling](#scheduling)
   - [Scheduler Algorithm](#scheduler-algorithm)
   - [Context Switching](#context-switching)
   - [Idle Process](#idle-process)
5. [Dependencies](#dependencies)
6. [Future Extensions](#future-extensions)

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
| `name` | `char[32]` | Process name. |
| `state` | `process_state_t` | Current state (READY, RUNNING, etc.). |
| `wakeup_tick` | `uint64_t` | Timer tick when a sleeping process should wake. |
| `context` | `process_context_t` | Saved CPU state for context switching. |
| `stack_base` | `uint64_t` | Base address of the user stack. |
| `stack_size` | `uint64_t` | Size of the user stack. |
| `ring0_stack` | `uint64_t` | Kernel stack pointer (for ring 0). |
| `ring0_stack_pages` | `uint64_t` | Pages allocated for the kernel stack. |
| `page_table` | `uint64_t` | Physical address of the PML4 table. |
| `next` | `struct process*` | Pointer to the next process in the circular list. |

### Process States

| State | Description |
|-------|-------------|
| `PROCESS_READY` | The process is ready to run but not currently executing. |
| `PROCESS_RUNNING` | The process is currently executing on the CPU. |
| `PROCESS_BLOCKED` | The process is waiting for an event (I/O, etc.). |
| `PROCESS_SLEEPING` | The process is sleeping until `wakeup_tick`. |
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

1. Mark the current process as `PROCESS_TERMINATED`.
2. Call `schedule()` to switch to another process.

**Note:** The process is not immediately freed. It is reaped by `process_reap()`.

### Process Reaping

**Function:** `process_reap()`

1. Iterate through the process list.
2. Find processes in the `PROCESS_TERMINATED` state.
3. Remove them from the list and free their memory.

**Reaping Strategy:**
- The kernel calls `process_reap()` periodically (e.g., in the idle loop).
- The idle process is never reaped.

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
| Scheduler | PIT, Process Manager | Timer interrupts for scheduling. |
| Context Switch | GDT, TSS | User/kernel mode transitions. |

---

## Conclusion

The ELF loader and process management subsystem provides the foundation for running user programs. The ELF loader supports 64-bit executables, and the process manager provides process creation, scheduling, and termination. The cooperative scheduler is simple but effective for a hobby OS.

For more details, refer to the source code in `system/elf/`, `system/process/`, and `system/cpu/` (for TSS and context switching).

---

**Document Version:** 1.0  
**Last Updated:** September 2026  
**Project:** LufiraOS