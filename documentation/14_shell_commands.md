# Shell and Built-in Commands

This document describes the interactive shell and the built-in command set of LufiraOS. The shell provides a command-line interface for system control, file management, process control, users/permissions, USB mass storage, networking, and audio playback.

---

## Table of Contents

1. [Overview](#overview)
2. [Shell Architecture](#shell-architecture)
   - [Command Loop](#command-loop)
   - [Line Editing](#line-editing)
   - [Command History](#command-history)
   - [Tab Completion](#tab-completion)
   - [Current Working Directory](#current-working-directory)
3. [Command Categories](#command-categories)
   - [System Commands](#system-commands)
   - [Colour Commands](#colour-commands)
   - [Filesystem Commands](#filesystem-commands)
   - [Process Commands](#process-commands)
   - [Users & Permissions Commands](#users--permissions-commands)
   - [USB Mass Storage Commands](#usb-mass-storage-commands)
   - [Network Commands](#network-commands)
   - [Audio Commands](#audio-commands)
4. [Command Reference](#command-reference)
5. [Integration with Kernel Components](#integration-with-kernel-components)
   - [Keyboard Driver](#keyboard-driver)
   - [Filesystem (VFS/LufiraFS)](#filesystem-vfslufirafs)
   - [Process Manager and ELF Loader](#process-manager-and-elf-loader)
   - [Users and Permissions](#users-and-permissions)
   - [USB Mass Storage and FAT](#usb-mass-storage-and-fat)
   - [Networking](#networking)
   - [Audio (AC'97)](#audio-ac97)
   - [ACPI](#acpi)
   - [Developer Mode and Logging](#developer-mode-and-logging)
6. [Conclusion](#conclusion)

---

## Overview

The LufiraOS shell is a simple, interactive command interpreter that runs as a user process. It provides:

- **Command-line interface** – users type commands and see output.
- **Line editing** – insert/delete characters, cursor movement.
- **Command history** – up to 20 commands stored, navigation with arrow keys.
- **Tab completion** – auto-completes command names.
- **Current working directory** – per-process state (see below), not a shell-only concept.

**Design Philosophy:**
- **Simplicity** – the shell is easy to understand and extend.
- **Responsiveness** – it reacts quickly to user input.
- **Integration** – it leverages all kernel subsystems, and now routes its filesystem commands through the same `vfs_*_at()` layer ring-3 programs use via syscalls (see [Filesystem (VFS/LufiraFS)](#filesystem-vfslufirafs)), instead of duplicating LufiraFS calls.

---

## Shell Architecture

### Command Loop

The shell operates in a continuous loop:

1. Display the prompt: `[lufiraos@kernel] <cwd> $`
2. Read user input character by character.
3. Process special keys (backspace, arrows, enter, tab).
4. On enter, execute the command.
5. Display the result and return to the prompt.

### Line Editing

The shell provides full line editing capabilities:

| Key | Action |
|-----|--------|
| Regular character | Insert at cursor position. |
| Backspace | Delete character before cursor. |
| Left arrow | Move cursor left. |
| Right arrow | Move cursor right. |
| Home/End | Move to start/end of line (not yet supported). |
| Enter | Execute the command. |

**State Variables:**
- `current_line[INPUT_BUFFER_SIZE]` – the editable line.
- `current_line_length` – length of the line.
- `cursor_position_in_line` – cursor index.
- `command_start_x` and `command_start_y` – prompt position on screen.

### Command History

The shell stores a history of executed commands:

- **Storage:** `command_history[HISTORY_SIZE][INPUT_BUFFER_SIZE]`
- **Navigation:** Up arrow for older commands, down arrow for newer commands.
- **Deduplication:** Consecutive identical commands are not stored.
- **Retrieval:** `get_history_command(index)` returns a command from history.

**History Flow:**
1. User presses Up arrow → load previous command.
2. User presses Down arrow → load next command (or blank line).
3. When Enter is pressed → add current command to history.

### Tab Completion

Tab completion matches the current input against a list of built-in commands:

1. **Find Matches** – search the command list for commands starting with the current input.
2. **Single Match** – auto-complete the command and add a space.
3. **Multiple Matches** – display all matches and re-display the prompt.

**Command List** (53 entries, `shell_handle_tab()` in `shell.c`): `help, clear, reboot, shutdown, version, echo, history, status, trap, color, colors, fg, bg, reset, pwd, cd, ls, mkdir, rm, touch, cat, cp, mv, rename, edit, run, runbg, exec, write, beep, mixer, music, kill, wait, ps, df, du, devmode, whoami, chmod, chown, useradd, groupadd, su, usbinfo, usbread, usbwrite, mount, unmount, ifconfig, ping, wget`.

### Current Working Directory

The current working directory is **per-process** state, not a shell-global — `cwd_path`/`cwd_inode` in `kernel/shell/shell.h` are `#define`d as macros expanding to `current_process->cwd_path`/`current_process->cwd_inode` (`kernel/system/process/process.h`). This is the same state `SYS_GETCWD`/`SYS_CHDIR` read and write for ring-3 programs (see [`13_syscalls.md`](13_syscalls.md#filesystem--identity)) — the shell's own `cd` and a program's `chdir()` syscall are the same notion of "current directory" for that process, not two independently-drifting ones. A forked child inherits its parent's cwd; `exec()` preserves it (POSIX semantics); a freshly-created process starts at the LufiraFS root.

| Field | Description |
|-------|-------------|
| `cwd_path[256]` | The current directory path as a string (e.g., `/home/user`). |
| `cwd_inode` | The LufiraFS inode number of the current directory (`LUFIRAFS_ROOT_INODE` = root by default). |

**Commands that use CWD:** `pwd`, `cd`, `ls`, `mkdir`, `rm`, `touch`, `cat`, `write`, `edit`, `cp`, `mv`, `run`/`runbg` (relative filenames).

---

## Command Categories

### System Commands

| Command | Description |
|---------|-------------|
| `help` | Displays all available commands. |
| `clear` | Clears the console screen and shows the prompt. |
| `reboot` | Flushes the filesystem and reboots the system. |
| `shutdown` | Flushes the filesystem and attempts ACPI shutdown. |
| `version` | Displays kernel version and build date. |
| `status` | Shows interrupt flag and CPU state. |
| `trap` | Triggers test exceptions (`int3`, `ud2`, `pf`, `cli`, `sti`, `hlt`). |
| `echo` | Prints the given text. |
| `history` | Displays the command history. |
| `devmode [on\|off]` | Shows or toggles developer mode (verbose driver/boot output) — see [`04_logging.md`](04_logging.md). |

### Colour Commands

| Command | Description |
|---------|-------------|
| `color <fg> [bg]` | Sets foreground/background using palette index (0–255) or RGB (6-digit hex). |
| `colors` | Shows the 16-colour palette table and usage examples. |
| `fg <color>` | Sets only foreground colour. |
| `bg <color>` | Sets only background colour. |
| `reset` | Resets colours to default (white on black). |

**Colour Formats:**
- **Palette Index:** 1 or 2-digit hex (e.g., `0F` = white on black).
- **RGB:** 6-digit hex (e.g., `FF0000` = red).

**Examples:**
- `color 1F` – blue background, white text.
- `color FF0000` – red text on current background.
- `color 0000FF 00FF00` – blue text on green background.
- `fg 4` – red text.
- `bg 1` – blue background.

### Filesystem Commands

| Command | Description |
|---------|-------------|
| `pwd` | Prints the current working directory. |
| `cd <dir>` | Changes the current directory (supports `..`). |
| `ls [-l] [path]` | Lists directory contents, colourised (directories vs. files). Defaults to the current directory; `-l` shows long format (permissions, owner/group, type, size, name); `[path]` lists a different directory. |
| `mkdir <name>` | Creates a new directory (requires write+exec on the parent). |
| `rm <name>` or `rm *` | Removes a file or empty directory. `rm *` removes all items in the current directory. |
| `touch <filename>` | Creates an empty file. |
| `cat <file>` | Displays the contents of a file (requires read permission). |
| `write <file> <text>` | Writes text to a file (creates or overwrites). |
| `edit <file> <text>` | Appends text (with newline) to a file. |
| `cp <src> <dst>` | Copies a file. |
| `mv <src> <dst>` | Moves/renames a file. |
| `rename <old> <new>` | Renames a file (alias for `mv`). |
| `df` | Shows LufiraFS free/used space and inode counts. |
| `du [path]` | Shows disk usage (blocks actually occupied on disk) of a file or directory, defaulting to the current directory. |

### Process Commands

| Command | Description |
|---------|-------------|
| `run <filename> [args...]` | Loads an ELF file and runs it in the foreground (blocks the shell), passing any extra words as `argv`. |
| `runbg <filename> [args...]` | Same, but in the background (shell returns immediately). |
| `exec <filename>` | Replaces the shell's own process image with the given ELF program — does not return on success. |
| `ps` | Lists running processes with PID, state, and name. |
| `kill [-SIGNAL] <pid>` | Sends a signal to a process (`-TERM`/`-KILL`/`-STOP`/`-CONT`, or numeric; default `-TERM`). |
| `wait <pid>` | Blocks until the given child process exits, then prints its exit code. |

### Users & Permissions Commands

| Command | Description |
|---------|-------------|
| `whoami` | Shows the current user and group (`uid`/`gid` and their names). |
| `chmod <mode> <path>` | Changes a file's permission bits (octal, e.g. `644`). Owner or root only. |
| `chown <user>[:group] <path>` | Changes a file's owner (and optionally group). Root only. |
| `useradd <user> <password> [group]` | Creates a new user, appended to `/etc/passwd`. |
| `groupadd <group>` | Creates a new group, appended to `/etc/group`. |
| `su <user> [password]` | Switches the shell's own `uid`/`gid` to another user. Root needs no password for any target; anyone else needs the target's own password. |

See [Users and Permissions](#users-and-permissions) below and [`15_users_permissions.md`](15_users_permissions.md) for the full model.

### USB Mass Storage Commands

| Command | Description |
|---------|-------------|
| `usbinfo` | Lists detected USB storage devices (capacity, block size). |
| `usbread <device> <lba>` | Reads one 512-byte block and shows a hex dump. |
| `usbwrite <device> <lba> <text>` | Writes text into one block. |
| `mount <device>` | Mounts a USB device's FAT12/16/32 filesystem (reads the whole device into RAM, capped at 8 MB) and lists its root directory. |
| `unmount` | Flushes any modified sectors back to the device and unmounts it. |

See [USB Mass Storage and FAT](#usb-mass-storage-and-fat) below.

### Network Commands

| Command | Description |
|---------|-------------|
| `ifconfig [ip] [netmask] [gateway]` | Shows the current network configuration and MAC address, or sets a new static configuration. |
| `ping <ip> [count]` | Sends ICMP echo requests and reports round-trip time / loss. |
| `wget <ip> <path> [file]` | Downloads a file over HTTP from a literal IP address (no DNS) and saves it via LufiraFS. |

See [Networking](#networking) below.

### Audio Commands

| Command | Description |
|---------|-------------|
| `beep` | Plays a 440 Hz tone for 250 ms. |
| `mixer [0-100]` | Sets the volume (without args, shows current state). |
| `music` | Plays a short test melody (C4–G4). |

---

## Command Reference

| Command | Syntax | Description |
|---------|--------|-------------|
| `help` | `help` | Displays all commands. |
| `clear` | `clear` | Clears screen and shows prompt. |
| `reboot` | `reboot` | Flushes LufiraFS and reboots. |
| `shutdown` | `shutdown` | Flushes LufiraFS and shuts down. |
| `version` | `version` | Shows kernel version. |
| `status` | `status` | Shows interrupt and CPU status. |
| `trap` | `trap <type>` | Triggers exception (`int3`, `ud2`, `pf`, `cli`, `sti`, `hlt`). |
| `echo` | `echo <text>` | Prints the argument. |
| `history` | `history` | Shows command history. |
| `devmode` | `devmode [on\|off]` | Shows/toggles developer mode. |
| `color` | `color <fg> [bg]` | Sets colours (hex index or RGB). |
| `colors` | `colors` | Shows colour palette. |
| `fg` | `fg <color>` | Sets foreground only. |
| `bg` | `bg <color>` | Sets background only. |
| `reset` | `reset` | Resets to default colours. |
| `pwd` | `pwd` | Shows current directory. |
| `cd` | `cd <dir>` | Changes directory (supports `..`). |
| `ls` | `ls [-l] [path]` | Lists directory entries, colourised. |
| `mkdir` | `mkdir <name>` | Creates a directory. |
| `rm` | `rm <name>` or `rm *` | Removes a file/directory (or all). |
| `touch` | `touch <filename>` | Creates an empty file. |
| `cat` | `cat <filename>` | Displays file content. |
| `write` | `write <file> <text>` | Writes text to a file. |
| `edit` | `edit <file> <text>` | Appends text (with newline). |
| `cp` | `cp <src> <dst>` | Copies a file. |
| `mv` | `mv <src> <dst>` | Moves/renames a file. |
| `rename` | `rename <old> <new>` | Renames a file. |
| `df` | `df` | Shows filesystem free/used space. |
| `du` | `du [path]` | Shows disk usage of a file/directory. |
| `run` | `run <filename> [args...]` | Executes an ELF program in the foreground. |
| `runbg` | `runbg <filename> [args...]` | Executes an ELF program in background. |
| `exec` | `exec <filename>` | Replaces the shell's own process image. |
| `ps` | `ps` | Lists running processes. |
| `kill` | `kill [-SIGNAL] <pid>` | Sends a signal to a process (default `-TERM`). |
| `wait` | `wait <pid>` | Waits for a child process to exit. |
| `whoami` | `whoami` | Shows current user/group. |
| `chmod` | `chmod <mode> <path>` | Changes file permissions (octal). |
| `chown` | `chown <user>[:group] <path>` | Changes file owner (root only). |
| `useradd` | `useradd <user> <password> [group]` | Creates a new user. |
| `groupadd` | `groupadd <group>` | Creates a new group. |
| `su` | `su <user> [password]` | Switches user. |
| `usbinfo` | `usbinfo` | Lists detected USB storage devices. |
| `usbread` | `usbread <device> <lba>` | Reads one block, shows hex dump. |
| `usbwrite` | `usbwrite <device> <lba> <text>` | Writes text into one block. |
| `mount` | `mount <device>` | Mounts a FAT filesystem from a USB device. |
| `unmount` | `unmount` | Unmounts the currently mounted filesystem. |
| `ifconfig` | `ifconfig [ip] [netmask] [gateway]` | Shows/sets network configuration. |
| `ping` | `ping <ip> [count]` | Sends ICMP echo requests. |
| `wget` | `wget <ip> <path> [file]` | Downloads a file over HTTP (IP only, no DNS). |
| `beep` | `beep` | Plays a beep. |
| `mixer` | `mixer [0-100]` | Gets/sets audio volume. |
| `music` | `music` | Plays a test melody. |

---

## Integration with Kernel Components

### Keyboard Driver

The shell receives keyboard input through the keyboard driver:

1. Keyboard interrupt (IRQ1, PS/2) or per-tick poll (USB HID) → `keyboard_scancode_to_key()` / `usb_hid_keyboard_report()`.
2. Both paths funnel into the shared input dispatcher, `input_keyboard_event()` (see [`07_drivers.md` § Input Dispatcher](07_drivers.md#input-dispatcher)), which debounces duplicate keystrokes delivered by both input paths.
3. Shell handlers: `shell_handle_char()`, `shell_handle_backspace()`, `shell_handle_ctrl_c()`, etc.

**Key Mapping:**

| Key | Shell Function |
|-----|----------------|
| Regular key | `shell_handle_char()` |
| Backspace | `shell_handle_backspace()` |
| Enter | `shell_handle_enter()` |
| Left arrow | `shell_handle_left_arrow()` |
| Right arrow | `shell_handle_right_arrow()` |
| Up arrow | `shell_handle_up_arrow()` |
| Down arrow | `shell_handle_down_arrow()` |
| Tab | `shell_handle_tab()` |
| Ctrl+C | `shell_handle_ctrl_c()` — signals the foreground process (see [`12_elf_processes.md` § Signals and kill](12_elf_processes.md#signals-and-kill)). |

### Filesystem (VFS/LufiraFS)

Filesystem commands (`cp`, `mv`, `ls`, `mkdir`, `rm`, `touch`, `run`) call the same `vfs_*_at()` layer (`vfs_open_at`, `vfs_mkdir_at`, `vfs_unlink_at`, `vfs_lookup_at`, …) that syscalls use internally — see [`08_filesystem.md`](08_filesystem.md) — instead of calling LufiraFS directly, removing what used to be two independently-maintained copies of the same logic. The VFS layer itself does **no** permission checking; every command still makes its own explicit `check_perm()`/`lufirafs_check_access()` call before the `vfs_*_at()` call, exactly mirroring how the equivalent syscalls check permissions. `cat`, `write`, and `exec` are the exceptions that still go through the syscall-style path more directly (`exec` always did, via `do_exec()`).

- `ls`/`du` – use `vfs_lookup_at()` + `vfs_open_at()` + `vfs_readdir()`; `ls -l`'s detail columns (permissions, owner/group name, size) still read `lufirafs_read_inode()` directly per entry, since `vfs_dirent_t` doesn't carry that detail — a known, accepted gap (would need a `stat`-equivalent VFS primitive).
- `cd` – uses `lufirafs_lookup()` (a real `..`/`.` directory entry, not a special case) and updates the process's own `cwd_inode`/`cwd_path`.
- `mkdir`/`touch` – `vfs_mkdir_at()` / `vfs_create_at()`.
- `rm` – `vfs_unlink_at()`; the `rm *` bulk-delete path is a separate function (`command_rm_all()`) using a heap-allocated name buffer rather than a large stack array, to stay within the shell process's 16 KB stack once the VFS call chain's own frame depth is added on top.
- `cat` – `lufirafs_lookup()`/`lufirafs_read_inode()`/`lufirafs_read()`, allocating exactly `inode.size` bytes.
- `write` – creates/truncates then writes.
- `edit` – appends at the file's current end.
- `cp`/`mv` – `vfs_open_at()` + `vfs_read()`/`vfs_write()` + `vfs_close()`, and for `mv`, `vfs_unlink_at()` on the source.
- `df` – reads free/used block and inode counts straight from the superblock.

### Process Manager and ELF Loader

Process commands use the process manager and ELF loader — see [`12_elf_processes.md`](12_elf_processes.md):

- `run`/`runbg` – split any extra words into a real `argv[]` (`split_argv()`) and call `elf_exec()`/`elf_exec_background()`.
- `exec` – calls `do_exec()` → `elf_exec_replace()` (in-place, does not return on success) with a synthetic single-element `argv`.
- `ps` – calls `process_ps()` to list all processes.
- `kill` – calls `process_signal()` with the requested signal (`process_kill()` is just `process_signal(pid, SIGKILL)`).
- `wait` – calls `process_wait()`, blocking until the given child exits.

### Users and Permissions

`kernel/shell/commands/users.c` implements this category — see [`15_users_permissions.md`](15_users_permissions.md) for the on-disk format and permission model in full:

- `whoami` – looks up `current_process->uid` in the in-memory user table.
- `chmod`/`chown` – resolve the target path, then write the inode's `perm`/`uid`/`gid` fields directly.
- `useradd`/`groupadd` – append a new line to `/etc/passwd`/`/etc/group` and update the in-memory table immediately (no reboot needed).
- `su` – mutates `current_process->uid`/`gid` directly in place, the same way `cd` mutates `cwd_inode` — not a syscall, since only the shell's own identity needs to change.

### USB Mass Storage and FAT

`kernel/shell/commands/usb.c` (`usbinfo`/`usbread`/`usbwrite`) and `kernel/shell/commands/mount.c` (`mount`/`unmount`) talk directly to the xHCI Mass Storage block API (`xhci_msd_*`, see [`07_drivers.md`](07_drivers.md#usb-xhci)) and, for `mount`, the FAT driver (`kernel/fs/fat/fat.c`) — bypassing the VFS entirely, since LufiraFS is the only VFS-registered filesystem. `mount` reads the whole device into a `kmalloc()`'d buffer (capped at 8 MB — the kernel heap is 16 MB total) and calls `fat_init()`; `unmount` walks the FAT driver's dirty-sector bitmap and writes each changed sector back with `xhci_msd_write_block()` directly (not `fat_flush()`/`fat_sync()`, which hardcode the ATA disk driver and would otherwise corrupt the LufiraFS-bearing disk image).

### Networking

`kernel/shell/commands/net.c` implements `ifconfig`/`ping`/`wget` on top of the polled network stack (`kernel/net/`, `kernel/drivers/net/rtl8139.c`) — see [`16_networking.md`](16_networking.md):

- `ifconfig` – reads/writes the single global `net_config_t` (IP/netmask/gateway).
- `ping` – `icmp_send_echo_request()` plus a bounded poll-wait for the matching reply.
- `wget` – `tcp_connect()`/`tcp_send()`/`tcp_recv_poll()` a minimal HTTP/1.0 GET, streaming the response body to a file via the same create→write→sync sequence `write` uses.

### Audio (AC'97)

Audio commands use the AC'97 driver:

- `beep` – calls `ac97_play_tone(440, 250)`.
- `mixer` – calls `ac97_set_volume()` and `ac97_get_volume()`.
- `music` – plays a predefined melody using `ac97_play_tone()`.

### ACPI

The `shutdown` command uses ACPI:

- `acpi_shutdown()` – performs ACPI S5 shutdown.
- Falls back to legacy ports if ACPI is not available.

### Developer Mode and Logging

The `devmode` command wraps `devmode_set()`/`devmode_is_enabled()` from `system/devmode/` — see [`04_logging.md`](04_logging.md) for the full design. Regardless of developer mode, several commands (`run`, `runbg`) call `klog()` to record what they did to `/logs/system.log`, which can be read back with the ordinary `cat` command.

---

## Conclusion

The LufiraOS shell provides a functional command-line environment with a rich set of built-in commands (53 in total) spanning system control, file management, process control, users/permissions, USB mass storage, networking, and audio. Its tight integration with the console, keyboard, filesystem, process manager, and audio subsystems makes it a powerful tool for system control, development, and debugging. The simple, modular design allows for easy extension with new commands and features.

For more details, refer to the source code in `shell/` and `shell/commands/`.

---

**Document Version:** 2.0
**Last Updated:** September 2026
**Project:** LufiraOS
