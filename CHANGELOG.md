# Changelog

All notable changes to LufiraOS are documented in this file.

## [0.6.0] - 2026-09-24

Covers all changes since `v0.3.1` (commit `7da5d8a`, "fixes 1", inclusive).

### Added

- **Preemptive multitasking.** The scheduler now genuinely preempts ring-3 code from the timer (100 Hz, ~50 ms timeslice) instead of relying on a process to voluntarily yield; a new `context_enter_ring3()` cold-start path guarantees every process actually reaches ring 3 (a CPU-bound program that never calls a syscall previously never left ring 0). A misbehaving program can no longer freeze the whole system.
- **Anonymous memory mapping.** `mmap`/`munmap` are fully implemented (eager allocation, per-process region tracking, `PROT_READ`/`PROT_WRITE`/`PROT_EXEC` enforced via the NX bit) — no longer stubs. This is what the new minimal libc's `malloc()` is built on.
- **Syscall hardening.** Every syscall that dereferences a user pointer now validates it (rejects kernel addresses and unmapped memory instead of trusting user input blindly). Real errno-style error codes (`EFAULT`, `EINVAL`, `ENOENT`, `ENOTDIR`, `ERANGE`, `EPERM`, `EACCES`, …) replace bare `-1`. `getcwd`/`chdir` are real syscalls backed by per-process state instead of stubs. New filesystem syscalls `SYS_MKDIR`, `SYS_RMDIR`, `SYS_UNLINK`, `SYS_READDIR` expose functionality the VFS already had internally. Total syscall count: 19 → 27.
- **Real argv/envp.** `run`/`runbg`/`exec` now split and pass real command-line arguments through to user programs (`int main(int argc, char **argv, char **envp)`); previously every program always received zero arguments.
- **A minimal userspace libc** (`libc/`) — `crt0.S` startup, `malloc`/`free` (arena-based, built on `mmap`), a `string.h` subset, and a small `printf`. This is the first C toolchain pipeline in the project; every prior test program was hand-written NASM.
- **Users, groups, and permissions** — a Unix-like `uid`/`gid`/9-bit permission model. Persistent `/etc/passwd` and `/etc/group` (colon-separated, FNV-1a password hashing), new commands `whoami`, `chmod`, `chown`, `useradd`, `groupadd`, and `su`. Every file/directory operation (open, create, mkdir, rm, cp, mv, run, …) now enforces owner/group/other permission bits; root (`uid` 0) bypasses all checks. Boots straight into a root shell — no login prompt.
- **xHCI USB host controller**, replacing the old UHCI (USB 1.1) driver entirely — real command/event/transfer ring model, HID keyboard/mouse support carried over with feature parity.
- **USB Mass Storage** (Bulk-Only Transport over xHCI) — block-level `usbinfo`/`usbread`/`usbwrite` commands, and new `mount`/`unmount` commands that activate the (previously entirely dead, never-compiled) FAT driver to mount a USB flash drive's FAT12/16/32 filesystem read/write. Not integrated into the VFS (LufiraFS stays the only VFS-mounted filesystem) — `mount` reads the whole device into RAM (capped at 8 MB) and works directly against the FAT driver, the same way `usbread`/`usbwrite` talk to the block layer directly.
- **RTL8139 network driver and a basic TCP/IP stack** — Ethernet, ARP, IPv4, ICMP, and a minimal single-connection blocking TCP client, entirely polled (no APIC/MSI support exists in this kernel). New commands `ifconfig`, `ping`, and `wget` (IP-address targets only, no DNS).
- New filesystem syscalls exposed to shell commands via a `vfs_*_at()` layer (`cp`/`mv`/`ls`/`mkdir`/`rm`/`touch`/`run` now go through the same syscall-layer VFS calls that ring-3 programs use, instead of calling LufiraFS internals directly — removing the prior code duplication between the two paths).
- Process teardown now actually frees a process's address space (page tables), ring-0 stack, and open file descriptors on exit — previously every `run`/`runbg`/`exec` leaked all three. Orphaned background (`runbg`) processes with no `wait()` are now reaped automatically from the idle loop. `MAX_PROCESSES` is now enforced (previously defined but never checked).

### Fixed

- `cd`, `mkdir`, `cp`, `mv`, `exec`, `fork()` (a register-clobber bug in the ring-3 entry path), and a small process-management race — the batch of stability fixes that opened this cycle (`fixes 1`, `fix mkdir and cd`, `fix`, `fix fork 2`).
- A page-table-permission bug (`get_or_create_table()` never set `PAGE_USER` on newly-created intermediate paging structures) that silently made every freshly-`mmap`'d or stack page kernel-only accessible from ring 3 — invisible until real ring-3 code first tried to read/write data through `mmap`.
- Two xHCI USB Mass Storage reliability bugs found while stress-testing the new `mount` command (the first code in the project to issue thousands of bulk transfers back-to-back): a bulk-transfer timeout that was five times shorter than the control-transfer timeout (a forgotten debugging artifact), and a real ring-buffer bug — the transfer ring's Link TRB never had its Cycle bit updated on wraparound, so the controller would stall at the boundary of every ~255 transfers. A third race (the timer-interrupt-driven USB poll could steal the completion event a synchronous Mass Storage wait was blocked on) was also found and fixed.
- `command_rm`'s bulk-delete path (`rm *`) and its single-file path could triple-fault — GCC reserves a function's entire worst-case stack frame up front (no `-O`), so a large stack-local array inside a rarely-taken branch still consumed stack on every call, and the newly-added VFS call chain tipped an already-marginal 16 KB shell stack over the edge.
- `lufirafs_create()`'s VFS wrapper collapsed every failure reason to the same generic error code instead of passing the real one through.
- Various boot-path, HID input, and process-lifecycle fixes bundled in `bug fixes 1`.

### Changed

- Kernel version bumped to **0.6.0**.
- Shell filesystem commands (`cp`, `mv`, `ls`, `mkdir`, `rm`, `touch`, `run`) migrated from calling LufiraFS internals directly to the same `vfs_*_at()` layer the syscalls use, while explicitly keeping their existing permission checks (the VFS layer itself does no permission enforcement).
- Trimmed and corrected a number of oversized or stale comment blocks across the kernel (including doc comments that still referred to the removed UHCI driver or a cooperative-only scheduler).

### Known Issues

- **`mount` reads the whole USB device into RAM up front** (capped at 8 MB) — no lazy/streaming FAT access, and no VFS integration (files on a mounted USB drive aren't reachable through `cat`/`cp`/`ls` yet, only through `mount`'s own directory listing).
- **The bootloader's own FAT loader and the legacy `kernel/fs/fat/fat.c` are two independent FAT implementations** that happen to now both be in active use (ESP loading vs. USB mount) — not unified.
- **No DNS or DHCP.** `wget`/`ifconfig` work with literal IP addresses and a static network configuration only.
- **Networking has no retransmission or congestion control.** The TCP client is a minimal, single-connection, best-effort implementation suited to a local/QEMU link, not a real network.
- **Real hardware remains untested.** The system is developed and tested exclusively in QEMU.
- No package manager yet — every driver and shell command still ships built into the kernel binary. This is the explicit scope of the next release (v0.7).


## [0.3.1] - 2026-09-13

### Fixed

- `cd`, `cp`, `du`, and `mv` commands 
- `exec` command path bug

### Known Issues

- Holding down a keyboard key does not repeat the character.
- `fork()` and `exec()` are unreliable — a forked child process can crash before it reaches its first instruction.


## [0.3.0] - 2026-09-12

Covers all changes since `v0.1.0` (commit `1fa52bd`, "fix bug with clear", inclusive).

### Added

- **LufiraFS** — a custom filesystem that replaces FAT as the primary storage backend. FAT is now used only for the small UEFI ESP partition that firmware requires to load the bootloader and kernel; the rest of the disk is a dedicated LufiraFS region with its own superblock, block bitmap, fixed inode table, and real `.`/`..` directory entries (fixing FAT's root-only directory search and hardcoded-parent-cluster bugs). Ships with a host-side `tools/mkfs_lufirafs` formatting/populating tool and full VFS integration.
- `df` and `du` shell commands for filesystem free-space and disk-usage reporting.
- USB support: a UHCI host-controller driver and a USB HID boot-protocol keyboard/mouse driver, unified with the existing PS/2 input through a new `drivers/input` dispatcher.
- Real process primitives: `fork()`, in-place `exec()`, `wait()`, signal-based `kill` (`SIGTERM`/`SIGKILL`/`SIGSTOP`/`SIGCONT`), and anonymous pipes, exposed as new syscalls (`SYS_FORK`, `SYS_WAIT`, `SYS_KILL`, `SYS_PIPE`) and shell commands (`kill`, `wait`, `exec`).
- Per-process address-space cloning for `fork()`, and a private low-memory identity map per process so that mapping a user ELF segment in one process can no longer corrupt the identity map used by others.
- `Ctrl+C` to interrupt the current foreground process from the shell.
- **Developer mode** — a persistent on-disk flag (`/system/devmode.flag`) that gates verbose boot and driver diagnostics. Toggle with the new `devmode [on|off]` shell command; `make debug` now enables it automatically for every debug run.
- **Kernel logging** (`klog`) — short status lines are recorded to `/logs/system.log` for boot and process events, so they remain available even when developer mode is off.
- New root directories `/system` (developer-mode flag) and `/logs` (system log), created when the disk image is built.
- A graphical boot splash (large scaled "LufiraOS" wordmark + caption) shown while the system boots quietly, and a small tilted "LufiraOS" watermark drawn once the shell starts.
- Colourised `ls` output (directories vs. files) and support for `ls [-l] <path>` — previously any path argument to `ls` was silently ignored and it always listed the current directory.

### Changed

- Kernel version bumped to **0.3.0**.
- The boot log is quiet by default: low-level status lines (GDT/TSS/IDT/PIC/PMM/paging/heap/LufiraFS mount/ACPI/process manager/PIT/syscalls/VFS/keyboard/mouse/AC'97/UHCI) now only print when developer mode is enabled. Genuine errors and warnings are always shown regardless of the mode.
- Introduced a dedicated kernel-space physical memory map (`phys_to_virt()`) used throughout paging/process/ELF code, replacing the previous assumption that low physical memory stayed identity-mapped under every process's page tables — that assumption broke as soon as a user ELF segment was mapped over the same physical range.
- `status` now reports whether interrupts have ever been enabled on this boot instead of the live EFLAGS.IF bit, which always read as cleared because shell commands run synchronously inside the keyboard IRQ handler.
- `write`/`edit` (and other filesystem commands) now resolve paths relative to the current working directory instead of always writing to the root directory.
- Cleaned up numerous oversized/disproportionate comment blocks across the kernel while preserving the non-obvious "why" behind each one.

### Fixed

- `clear` no longer corrupts console/history state.
- `cp`/`mv` no longer corrupt data when the source and destination share the same filename.
- Sector writes now take effect without requiring a reboot.
- Assorted UHCI/USB stability fixes (control-transfer timeouts, port-reset sequencing, TD/QH pool exhaustion).
- Assorted process/paging fixes that made running ELF programs (`run`/`runbg`) more reliable, including a register-clobber bug in `context_switch()`.

### Known Issues

- Holding down a keyboard key does not repeat the character.
- `fork()` and `exec()` are unreliable — a forked child process can crash before it reaches its first instruction.
- `cd`, `cp`, and `mv` are not fully stable — will be fixed in the next release (v0.3.1).
