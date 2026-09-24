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
   - [Command-Line Arguments (argv/envp)](#command-line-arguments-argvenvp)
3. [Process Management](#process-management)
   - [Process Structure](#process-structure)
   - [Process States](#process-states)
   - [Process Creation](#process-creation)
   - [Process Identity (uid/gid)](#process-identity-uidgid)
   - [Process Termination](#process-termination)
   - [Process Reaping](#process-reaping)
4. [fork(), exec(), wait(), and Signals](#fork-exec-wait-and-signals)
   - [fork()](#fork)
   - [exec() (In-Place Replace)](#exec-in-place-replace)
   - [wait()](#wait)
   - [Signals and kill](#signals-and-kill)
   - [Pipes](#pipes)
   - [The Kernel-Space Physical Memory Map](#the-kernel-space-physical-memory-map)
   - [fork()/exec() Reliability](#forkexec-reliability)
5. [Scheduling](#scheduling)
   - [Scheduler Algorithm](#scheduler-algorithm)
   - [Preemption](#preemption)
   - [Context Switching](#context-switching)
   - [Cold Start: context_enter_ring3()](#cold-start-context_enter_ring3)
   - [Idle Process](#idle-process)
6. [Dependencies](#dependencies)
7. [Future Extensions](#future-extensions)

---

## Overview

The ELF loader and process management subsystem provides:

- **ELF64 Loading** – loads 64-bit ELF executables (ET_EXEC and ET_DYN) into memory.
- **Process Creation** – allocates processes with their own address spaces and stacks, and passes real `argv`/`envp` to them.
- **Scheduling** – round-robin scheduling, **preemptive**: the timer forcibly reclaims the CPU from ring-3 code once a process's timeslice expires (see [Preemption](#preemption)).
- **Context Switching** – saves and restores CPU state when switching between processes, including a dedicated cold-start path that guarantees every process actually reaches ring 3 (see [Cold Start: context_enter_ring3()](#cold-start-context_enter_ring3)).
- **Process Teardown** – exiting a process frees its page table, its ring-0 stack, and closes any file descriptors it still had open.

**Design Philosophy:**
- **Simplicity** – the scheduler uses a simple circular linked list.
- **Modularity** – the ELF loader is separate from process management.
- **Performance** – context switching is fast and efficient.
- **Safety over cleverness** – preemption is gated on the interrupted code's actual CPL (`frame->cs == 0x33`), so kernel code (IRQ/syscall handlers, the idle loop) is never preempted out from under itself.

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

### Command-Line Arguments (argv/envp)

`run`, `runbg`, and `exec` pass real `argv`/`envp` to the new program, delivered the way `libc/crt0.S`'s `_start` expects: `call main` with `argc`/`argv`/`envp` already sitting in `rdi`/`rsi`/`rdx` (SysV integer-argument registers), so a user program's `int main(int argc, char **argv, char **envp)` receives them exactly as `execve()`'s caller would expect, without crt0 having to unpack anything off the stack itself. This is possible only because `crt0.S` is entirely under the kernel's own control (it is not a foreign C runtime) — no instruction between `_start` and `call main` touches `rdi`/`rsi`/`rdx` other than `and $-16, %rsp` (stack realignment, since `process_create()`/`process_prepare_exec()` don't guarantee a 16-byte-aligned initial RSP) and `xor %ebp, %ebp`.

**Function:** `build_exec_stack(new_pml4, stack_top, argv, envp, rsp_out, argv_out, envp_out)` (`kernel/system/process/process.c`)

1. Counts `argc`/`envc` by scanning for the NULL terminator, capped at `MAX_EXEC_ARGS` (64). Fails (`-1`) if no terminator is found within that many entries.
2. Computes the total bytes needed for the string data plus both NULL-terminated pointer arrays; fails (`-1`) if it exceeds `MAX_EXEC_ARGS_BYTES` (4096).
3. Writes each `argv[i]`/`envp[i]` string onto the new process's own stack (via `write_user_bytes()`, which walks the target page table without switching `CR3`), growing down from `stack_top`.
4. Writes the NUL-terminated `argv[]` and `envp[]` pointer arrays below the string data, pointing at the just-written strings.
5. Returns the resulting stack pointer (`*rsp_out`, below everything just built — this is where the real user-mode stack starts growing from) and the virtual addresses of the two pointer arrays (`*argv_out`, `*envp_out`).

`MAX_EXEC_ARGS`/`MAX_EXEC_ARGS_BYTES` are defined in `kernel/system/process/process.h` and reserve a fixed budget at the top of the 16 KiB user stack, leaving `USER_STACK_SIZE - MAX_EXEC_ARGS_BYTES` (12 KiB) of real working stack.

The caller (`elf_exec_internal()` in `elf.c`, backing both `elf_exec()` and `elf_exec_background()`) then writes the returned `argc`/`argv_addr`/`envp_addr` directly into the new process's saved context (`proc->context.rdi`/`rsi`/`rdx`) before its first activation, so they are already in place when `context_enter_ring3()` restores registers and `iretq`s into `_start`.

**Ownership convention** (documented at the declarations in `kernel/system/elf/elf.h`):

| Function | argv/envp ownership |
|----------|---------------------|
| `elf_exec()` / `elf_exec_background()` | **Borrowed** — only read via `build_exec_stack()`; the caller keeps ownership and must free them. |
| `elf_exec_replace()` | **Taken** — freed internally (on every return path, success or failure) via `free_argv_envp()`. |

`free_argv_envp(argv, envp)` frees the `argv[]`/`envp[]` in the form built by `copy_user_string_array()` (`kernel/system/syscall/syscall.c`): one `kmalloc()` per string plus one `kmalloc()` for the pointer array itself. It is exported from `elf.c` (not `static`) so `do_exec()` (the `SYS_EXEC` handler) can also use it on its own early failure paths, before ownership has actually transferred to `elf_exec_replace()`.

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
| `mmap_regions` / `next_mmap_addr` | `mmap_region_t[MAX_MMAP_REGIONS]` / `uint64_t` | `SYS_MMAP`/`SYS_MUNMAP`-backed regions and the bump pointer for the next free address under `MMAP_AREA_START` (see [`13_syscalls.md`](13_syscalls.md)). Zeroed/reset at the same lifecycle points as the rest of the address space — set fresh in `process_create()`, copied 1:1 in `process_fork()`, and reset in `process_commit_exec()` since `exec()` replaces the whole address space. |
| `uid` / `gid` | `uint32_t` / `uint32_t` | Process identity — see [Process Identity (uid/gid)](#process-identity-uidgid). |
| `is_shell` | `int` | Marks the process currently "acting as" the interactive shell — see [Process Termination](#process-termination). |
| `first_run` | `int` | `1` until the process's very first activation, then always `0`. Routes that first activation through `context_enter_ring3()` (a genuine ring0→ring3 `iretq`) instead of the ordinary `context_switch()` — see [Cold Start: context_enter_ring3()](#cold-start-context_enter_ring3). |
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

**RUNNING → READY transitions:** besides a process voluntarily yielding (`process_sleep()`, blocking in `process_wait()`, an explicit `schedule()` call), a `RUNNING` process is now also forced back to `READY` by the timer when its preemption timeslice expires — see [Preemption](#preemption). This applies only to processes genuinely executing in ring 3; kernel-mode execution paths are never preempted this way.

### Process Creation

**Function:** `process_create(name, entry)`

1. Allocate and zero the process structure; fail (return `NULL`) if the process count is already at the `MAX_PROCESSES` cap (32, `kernel/system/process/process.h`) — enforced by a check at the top of `process_create()` against a `process_count` counter incremented/decremented alongside every process creation/reap.
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
7. Set `first_run = 1` when `entry == NULL` — i.e. when the caller (`elf_exec_internal()` in `elf.c`, `process_fork()`) will itself overwrite `context.rip` with a real user-mode ELF entry point afterwards. When `entry` is passed directly (e.g. `process_create("shell", shell_task)` in `kernel.c`, which is kernel code, not a user ELF), `first_run` stays `0` so the process starts via the ordinary `context_switch()` in ring 0, as before.
8. Set `uid`/`gid` to `current_process`'s `uid`/`gid` (or `0`/`0` if there is no current process yet, i.e. during early boot) — **inherited from the launching process, not reset to root**. See [Process Identity (uid/gid)](#process-identity-uidgid).
9. Add the process to the circular list.
10. Return the process pointer.

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

### Process Identity (uid/gid)

`process_t` carries `uid`/`gid` fields identifying which user a process runs as. There is no `euid`/`egid` — this kernel has no setuid bit, so "real" and "effective" identity are always the same. The full permission-checking model (how `uid`/`gid` gate filesystem/process operations) lives in [`15_users_permissions.md`](15_users_permissions.md); this section covers only how identity propagates across the process-lifecycle events owned by this document:

| Event | Identity behavior |
|-------|--------------------|
| `process_create()` (backs `run`/`runbg`) | New process **inherits** `uid`/`gid` from `current_process` (the launching process) — not reset to root. |
| `process_fork()` | Child gets an exact copy of the parent's `uid`/`gid` (POSIX `fork()` semantics). |
| `exec()` (`process_commit_exec()`) | Identity is **left untouched** — matches real `execve()`, since there is no setuid bit to trigger a change. |
| `process_init()` (idle process, PID 0) | Explicitly set to `uid = gid = 0` (root) — it is the identity root the very first shell process inherits from. |

The only way a process's identity changes after creation is the shell's `su` command, which mutates `current_process->uid`/`gid` directly (the same pattern `cd` uses for `cwd_inode` — no dedicated syscall).

### Process Termination

**Function:** `process_exit()`

1. Mark the current process as `PROCESS_TERMINATED` and record `exit_code`.
2. If the exiting process was `is_shell` (i.e. it was — or, after `exec()`, used to be — the interactive shell), immediately create a fresh `"shell"` process and mark it `is_shell` before switching away, so the system is never left without an interactive prompt. `exec()` replaces a process's image in place without forking, so if the shell itself exits or is killed, no other process is left to fall back to.
3. Wake a parent blocked in `wait()` for this PID, if any.
4. Call `schedule()` (or switch directly to the waiting parent) to switch to another process.

**Note:** The process is not immediately freed. It is reaped by `process_reap()` or by the parent's `wait()` call — both routes now go through real teardown, described below.

**Real teardown (`free_process_resources()`, `kernel/system/process/process.c`):** every path that actually destroys a `process_t` — `process_reap()` and `process_wait()`'s reap — calls this single function first:

1. Closes every still-open file descriptor in the process's `fd_table` via the normal `vfs_close()` path (by temporarily swapping `current_fd_table` to the dying process's table), which correctly drops `file_t`/inode reference counts and triggers pipe-orphan cleanup for any pipe ends the process still held open — not just abandoning them.
2. Frees the process's user address space with `free_user_address_space()` (`kernel/system/mm/paging.c`): it walks every present PML4/PDPT/PD/PT entry in the user half (indices 0–255) and frees each backing physical page with `pmm_free_page()`, including the intermediate table pages themselves. It specifically skips the still-unsplit identity-map huge page that can occupy part of PML4[0] (shared kernel identity map, never owned by the process) and any leaf PTE that is still the raw identity mapping (`leaf_phys == leaf_virt`) rather than a real allocation.
3. Frees the ring-0 (kernel-mode) stack's backing pages.

Previously only the bare `process_t` struct was freed — the page table, the ring-0 stack, and any open file descriptors all leaked on every process exit. That is no longer the case.

### Process Reaping

**Function:** `process_reap()`

1. Iterate through the process list.
2. Find processes in the `PROCESS_TERMINATED` state.
3. Remove them from the list, call `free_process_resources()` on them (see [Process Termination](#process-termination)), and `kfree()` the `process_t` itself.

**Reaping Strategy:**
- `process_reap()` is called from `timer_irq_handler()` (`kernel/system/timer/pit.c`), on every timer tick — not from the idle loop. This closes a gap where it previously existed but was never actually reachable: the scheduler almost never hands control to the idle process while any other `READY` process is alive (the shell always is), so waiting for a real switch into the idle loop (as the code once did) essentially never happened in practice, and every `run`/`runbg`/`exec`'d process's page table and file descriptors leaked forever once it exited without an explicit `wait()`. Calling it unconditionally from the timer tick — like `usb_poll()`/`net_poll()` — is safe because it neither blocks nor switches context.
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
2. Only once loading succeeds is the process switched over (`process_commit_exec()`): new `page_table`, new user stack, same PID, same position in the scheduler's list. The old address space (captured as `old_pml4` before the overwrite) is freed with `free_user_address_space()` right after — otherwise every `exec()` would leak the process's previous image (ELF/stack/mmap regions) forever.
3. Execution jumps directly into the new program via `context_enter_ring3()`, not the ordinary `context_switch()` — a successful `exec()` is always "the first instruction of a new program," the same cold-start case as a process's very first activation (see [Cold Start: context_enter_ring3()](#cold-start-context_enter_ring3)). This call never returns on success.

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

### fork()/exec() Reliability

`fork()`/`exec()` were previously flagged as unreliable — a forked child could crash (typically a triple fault) somewhere between the child being scheduled and it executing its first instruction, even after code bytes, stack contents, register values, and raw page-table entries were all verified correct. Two concrete bugs behind that have since been found and fixed in the source:

1. **Register clobber in the context-restore path.** In `context_switch()` (`kernel/system/process/switch.S`), RFLAGS was being staged through `%rax` immediately before the final `jmp *%r11`. For a freshly-forked child, `%rax` at that point *is* the fork return value (`0`, so the child should take the "child" branch) — routing RFLAGS through it clobbered that `0` with whatever RFLAGS happened to be (always non-zero), so the child always fell through to the "parent" branch instead. The fix pushes RFLAGS straight from memory (`pushq 136(%rsi)`) instead of via `%rax`.
2. **Missing `PAGE_USER` on intermediate page-table structures.** `get_or_create_table()` (`kernel/system/mm/paging.c`) used to create new intermediate PML4E/PDPTE/PDE entries with only `PAGE_PRESENT | PAGE_WRITE`, never `PAGE_USER`. x86-64 ANDs the U/S bit across every translation level, so a supervisor-only intermediate entry makes the entire address supervisor-only regardless of the leaf PTE's own flags — breaking ring-3 access to any freshly `mmap`'d or stack memory whose intermediate tables were only just created. The fix sets `PAGE_USER` on newly-created intermediate tables (and re-asserts it on existing ones on each lookup); the actual permission enforcement still comes from the leaf PTE's own flags, so this doesn't open up anything extra.

Both fixes are visible in the current source (with inline comments describing the failure mode at each site). No further reliability issues in `fork()`/`exec()` have surfaced in the code or its comments since; this document does not independently re-verify runtime behavior beyond what is confirmable by reading the source.

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

`schedule()` itself is invoked from multiple places — a process voluntarily yielding (`process_sleep()`, `process_wait()` blocking), `process_exit()`, and now, unlike before, the timer interrupt handler on every expired timeslice (see [Preemption](#preemption) below). The algorithm above is unchanged by that; only who *calls* it changed.

### Preemption

Multitasking is genuinely **preemptive**, not purely cooperative: `timer_irq_handler()` (`kernel/system/timer/pit.c`) counts down a fixed timeslice and forces a `schedule()` call when it expires.

```c
#define PREEMPT_TIMESLICE_TICKS 5   // 5 ticks @ 100 Hz PIT = ~50 ms
static uint32_t preempt_countdown = PREEMPT_TIMESLICE_TICKS;

if (frame->cs == 0x33 && current_process) {
    if (--preempt_countdown == 0) {
        preempt_countdown = PREEMPT_TIMESLICE_TICKS;
        schedule();
    }
}
```

The critical gate is `frame->cs == 0x33` — the segment selector for ring-3 user code. Preemption only fires when the code the timer interrupted was genuinely executing in ring 3 as user-mode process code; kernel code (syscall/IRQ handlers, the still-not-fully-reentrant keyboard/USB-in-timer path, the idle process's own `hlt` loop — which never enters ring 3 at all) is **never** preempted at an arbitrary point. This keeps kernel code exactly as reentrancy-safe as it was before preemption was added — the kernel is short-lived and already yields cooperatively where it needs to; the risk of preempting it at an arbitrary instruction is exactly what this gate avoids.

One consequence of genuine preemption: a CPU-bound program that never calls a syscall (e.g. an infinite compute loop) can no longer monopolize the CPU forever — it is forcibly switched out every `PREEMPT_TIMESLICE_TICKS` ticks like any other ring-3 process. Previously, the *only* transition back to ring 3 was a syscall's own `iretq` return path, so a process making zero syscalls never even reached ring 3 in the first place and ran unbounded in ring 0 — see [Cold Start: context_enter_ring3()](#cold-start-context_enter_ring3) for how every process is now guaranteed to reach ring 3 at all, independently of this preemption mechanism.

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

### Cold Start: context_enter_ring3()

Historically, the only code path that ever performed a real `iretq` back to ring 3 was a syscall's own return path (`syscall_entry.S`) — so a brand-new process ran its very first instructions with **kernel privilege** until (and unless) it made its first syscall. A process that never syscalls (an infinite compute loop, for instance) simply never transitioned to ring 3 at all.

**Function:** `context_enter_ring3(process_context_t *old, process_context_t *new)` (`kernel/system/process/switch.S`)

Same save half as `context_switch()` (byte-identical — all GPRs, RSP, RIP, RFLAGS, CR3 saved into `old`), but a different restore half: instead of a same-privilege `jmp` to the restored RIP, it builds a genuine `iretq` frame and transfers control through it:

1. Loads `CR3` from `new` (switches address space) before touching anything else.
2. Switches onto the *target* process's own ring-0 stack (`current_kernel_rsp`, set by `switch_to_process()` just before this call) — building the `iretq` frame on the *caller's* stack would page-fault, since that stack is mapped only under the old process's page table, not the one `CR3` was just switched to.
3. Pushes the `iretq` frame: user `SS` (`0x2B`), user `RSP` (`new->rsp`), `RFLAGS`, user `CS` (`0x33`), and `RIP` — the same selectors `syscall_entry.S` already uses for its own ring0→ring3 return.
4. Restores the general-purpose registers from `new` (RDI/RSI loaded from `new->rdi`/`new->rsi`, which is how `argc`/`argv` reach `main()` — see [Command-Line Arguments (argv/envp)](#command-line-arguments-argvenvp)).
5. `iretq`s — no scratch-register dance is needed here (unlike `context_switch()`'s `%r11` trick), since the transfer is a frame-driven `iretq`, not a `jmp` through a register.

**When it's used** — exactly the two situations that are "a new program's first instruction":

- **A process's very first activation.** `switch_to_process()` checks `process_t.first_run`: if set, it clears the flag and calls `context_enter_ring3()` instead of `context_switch()`. Every later resumption of that same process — including one preempted mid-execution straight out of ring 3 — goes through the ordinary `context_switch()`, because by then it is a real suspended call chain, not a cold start.
- **`exec()` replacing a process's image** (`elf_exec_replace()` in `elf.c`) — unconditionally, since a successful `exec()` is by definition "the first instruction of a new program," regardless of the process's own `first_run` state (which, for an already-running shell being `exec`'d over, would already be `0`).

### Idle Process

The idle process (PID 0) runs when no other process is ready. It is not a separate entry-point function: `process_init()` builds its `process_t` directly (bypassing `process_create()`) and makes it `current_process` immediately, representing the kernel's own boot-time execution context — its `page_table` is simply whatever `CR3` was active at that point, and `first_run` is explicitly left `0` (it has no ring-0 stack to build an `iretq` frame on, and it never enters ring 3 at all).

Its actual "run loop" is `kernel_main()`'s own tail loop in `kernel.c`:

```c
while (1) {
    asm volatile("sti");
    asm volatile("hlt");
    asm volatile("cli");
    schedule();
}
```

**Purpose:** The idle process uses minimal CPU time and allows the kernel to handle interrupts. In practice, `schedule()` almost never hands control to idle while any other process is `READY` (the shell always is) — see the note in [Process Reaping](#process-reaping) about why that made the old idle-loop reaping strategy unreliable.

---

## Dependencies

| Component | Depends On | Purpose |
|-----------|------------|---------|
| ELF Loader | PMM, Paging, Heap | Memory allocation and mapping. |
| Process Manager | PMM, Paging, Heap, GDT, TSS | Process creation and context switching. |
| fork()/exec()/wait()/kill | Process Manager, VFS | Address-space cloning, fd-table duplication, pipes. |
| Scheduler | PIT, Process Manager | Timer interrupts for scheduling and preemption. |
| Context Switch | GDT, TSS | User/kernel mode transitions, including cold-start (`context_enter_ring3()`) entry. |

---

## Conclusion

The ELF loader and process management subsystem provides the foundation for running user programs. The ELF loader supports 64-bit executables with real `argv`/`envp`, and the process manager provides process creation, preemptive scheduling, and real termination (page table, ring-0 stack, and file descriptors are all actually freed). The scheduler is simple — a circular linked list plus a fixed timeslice — but effective for a hobby OS.

For more details, refer to the source code in `system/elf/`, `system/process/`, and `system/cpu/` (for TSS and context switching).

---

**Document Version:** 2.0  
**Last Updated:** September 2026  
**Project:** LufiraOS