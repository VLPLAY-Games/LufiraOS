# Changelog

All notable changes to LufiraOS are documented in this file.

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
