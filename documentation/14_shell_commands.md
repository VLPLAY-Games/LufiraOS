# Shell and Commands

This document describes the interactive shell and command set of LufiraOS. **As of v0.7, the shell is no longer kernel code.** It is a real ring-3 ELF program, `shell.elf`, built in a separate sibling repository, [`lufira-packages`](https://github.com/VLPLAY-Games/lufira-packages) — and almost every command that used to be a kernel-native built-in is now its own small userspace package in that same repository, talking to the kernel purely through the syscall ABI documented in [`13_syscalls.md`](13_syscalls.md).

**What's still in this repository:** `kernel/shell/shell.c` and `kernel/shell/commands/*.c` still exist in the source tree and are still compiled into `kernel.bin` (see [`05_build_system.md`](05_build_system.md)) — but they are **dead code**. Nothing calls into them anymore; `kernel.c` spawns `/bin/shell.elf` directly on boot (see [`02_kernel_init.md`](02_kernel_init.md#23-create-shell-process)) and never falls back to anything kernel-native. Several commands that existed in that dead code (network, audio, and a handful of diagnostic commands) were never ported to userspace at all — see [What Didn't Make the Move](#what-didnt-make-the-move).

---

## Table of Contents

1. [Overview](#overview)
2. [Where Everything Actually Lives](#where-everything-actually-lives)
3. [The Shell (`shell.elf`)](#the-shell-shellelf)
   - [Command Loop](#command-loop)
   - [Line Editing](#line-editing)
   - [Command History](#command-history)
   - [Builtins vs. External Programs](#builtins-vs-external-programs)
   - [Current Working Directory](#current-working-directory)
   - [Ctrl+C and Foreground Jobs](#ctrlc-and-foreground-jobs)
4. [Shell Builtins](#shell-builtins)
5. [Packages (`/bin/*.elf`)](#packages-binelf)
   - [Filesystem](#filesystem)
   - [Process Control](#process-control)
   - [Users & Permissions](#users--permissions)
   - [USB Mass Storage](#usb-mass-storage)
   - [Color](#color)
   - [System / Power](#system--power)
   - [Package Management](#package-management)
6. [Command Reference](#command-reference)
7. [What Didn't Make the Move](#what-didnt-make-the-move)
8. [Integration with Kernel Components](#integration-with-kernel-components)
9. [Conclusion](#conclusion)

---

## Overview

The LufiraOS shell is a simple, interactive command interpreter that runs as an ordinary preemptible ring-3 process, like anything else. It provides:

- **Command-line interface** – users type commands and see output.
- **Line editing** – insert/delete characters at an arbitrary cursor position, left/right arrow movement.
- **Command history** – up to 16 commands, navigated with up/down arrows.
- **Current working directory** – per-process state (see below), not a shell-only concept.
- **Job control, minimally** – `fork()` + `SYS_SET_FOREGROUND` + `SYS_WAIT` gives Ctrl+C the ability to interrupt a foregrounded child; a trailing `&` runs anything in the background instead.

**Design Philosophy:**
- **Ordinary userspace process** – `shell.elf` has no special kernel privilege or hook. It reads `/dev/console` with a blocking `SYS_READ`, just like any other program could.
- **Thin builtins, real programs for everything else** – only things that must change the shell process's *own* state (`cd`, `su`, `mount`, history, …) are builtins. Everything else is a `/bin/*.elf` found by a PATH-like search and run via `fork()`+`SYS_EXEC`+`SYS_WAIT`.
- **One parser, no special-casing per program** – the shell doesn't know what `cp` or `ls` do; it just resolves a name to a path and runs it with whatever `argv` followed.

---

## Where Everything Actually Lives

| Layer | Lives in | Status |
|-------|----------|--------|
| `kernel/shell/shell.c`, `kernel/shell/commands/*.c` | **this repository** | Dead code. Still compiled into `kernel.bin`, never called. `kernel.c` spawns `/bin/shell.elf` directly; see [`02_kernel_init.md`](02_kernel_init.md#23-create-shell-process). |
| `shell.elf` (the real interactive shell) | `lufira-packages/shell/shell.c` | Live. Loaded straight off LufiraFS by the kernel on every boot and every shell respawn — not installed as a `dlpg` package (`dlpg` has no way to "remove" it, and removing it would make the system unbootable). |
| Coreutils and system commands (`cp`/`mv`/`ls`/`mkdir`/`rm`/`cat`/`touch`/`write`/`chmod`/`chown`/`kill`/`ps`/`color`/`reset`/`fg`/`bg`/`reboot`/`shutdown`/`devmode`/user-group management/USB block tools) plus `dlpg` | `lufira-packages/base/*.c` | Live. Each is a separate, dynamically-linked `.elf`, installed under `/bin/` (see [`17_package_manager.md`](17_package_manager.md)). |
| `df`/`du`/`free`/`cpuload` | `lufira-packages/user/*.c` | Live. Same deal, a different source subdirectory. |
| The GUI stack (window manager, terminal, notepad, files, calc, sysinfo) | `lufira-packages/apps/*.c` | Live, but not part of the text shell at all — see [`18_gui_wm.md`](18_gui_wm.md). |

---

## The Shell (`shell.elf`)

### Command Loop

1. Print the prompt: `[<user>@lufiraos] <~cwd-or-full-path> $` (home-relative with a `~` prefix when the cwd is inside the current user's home directory, otherwise the full path).
2. Read `/dev/console` one byte at a time via a blocking `sys_read(0, &c, 1)`.
3. On a regular printable byte, insert it at the cursor and redraw the line from that point; on backspace, delete the character before the cursor; on the escape codes the console driver sends for arrow keys (`0x01`/`0x02` left/right, `0x03`/`0x04` up/down — see `userspace/common/console.h`), move the cursor or walk history; Tab is accepted but does nothing (no completion in this version).
4. On Enter, split the line into `argv[]` on spaces (no quoting support) and dispatch.

There is no separate "read the whole line, then parse" step distinct from the editing loop — editing and echo happen character-by-character as bytes arrive, same as any real terminal-backed shell.

### Line Editing

| Key | Action |
|-----|--------|
| Regular character | Insert at cursor position, redraw the line's tail. |
| Backspace / `0x7F` | Delete the character before the cursor. |
| Left / Right arrow | Move the cursor without changing the line's content (`con_cursor_left()`/`con_cursor_right()` — a real cursor move, not a redraw-with-same-character trick, which was a past source of a display bug). |
| Up / Down arrow | Walk command history, replacing the current line. |
| Enter | Execute the command. |
| Tab | Accepted, does nothing (no tab completion in this version). |

### Command History

- Up to 16 entries (`HISTORY_SIZE`), stored in-process — lost on exit, not a file.
- A command identical to the immediately preceding one is not stored again (simple de-duplication, not full de-duplication across the whole history).
- `history` builtin lists them, 1-indexed.

### Builtins vs. External Programs

`dispatch()` checks the first word against a short, fixed list of builtins (`cd`, `pwd`, `exit`/`logout`, `help`, `history`, `clear`, `su`, `mount`, `unmount`, `echo`, `wait`) before falling through to external-program resolution. `run`/`runbg`/`exec` are legacy-compatible synonyms that just skip the first word and otherwise go through the same external-program path as typing the program's name directly — they are **not** required.

**Resolving an external name:** if it starts with `/`, it's used as-is (after confirming the file exists). Otherwise the shell tries, in order: `/bin/<name>`, then `/bin/<name>.elf`. There is no `$PATH` variable — the search list is hardcoded to `/bin/`.

**Running it:** `fork()`, then the child calls `sys_exec()` on the resolved path with the remaining words as `argv`. A trailing `&` on the command line (checked before resolution) runs it in the background — the shell just prints the child's PID and returns to the prompt immediately, without `SYS_WAIT`ing. Otherwise, the shell calls `SYS_SET_FOREGROUND(child_pid)`, blocks on `SYS_WAIT(child_pid)`, then clears the foreground marker (`SYS_SET_FOREGROUND(0)`) once it returns.

### Current Working Directory

The current working directory is **per-process** state, not a shell-global — `current_process->cwd_path`/`cwd_inode` (`kernel/system/process/process.h`), read and written by `SYS_GETCWD`/`SYS_CHDIR` (see [`13_syscalls.md`](13_syscalls.md#filesystem--identity)). This is exactly the same state whether it's the shell's own `cd` builtin or any program calling `chdir()` through the syscall wrapper — there's no separate "shell cwd" concept. A forked child inherits its parent's cwd; `exec()` preserves it (POSIX semantics); a freshly-created process starts at the LufiraFS root.

**Programs that resolve relative paths against it:** every filesystem package that takes a path argument (`cat`, `cp`, `mv`, `ls`, `mkdir`, `rm`, `touch`, `write`) does so via a shared helper, `resolve_path()` (`lufira-packages/common/pathutil.h`) — because `SYS_OPEN` itself only ever resolves from the LufiraFS root (see [`13_syscalls.md`](13_syscalls.md#file-operations)), so a relative argument has to be glued onto `SYS_GETCWD`'s result by the *calling program*, not the kernel. `SYS_MKDIR`/`SYS_RMDIR`/`SYS_UNLINK`/`SYS_CHDIR` are the exception — those resolve relative to `cwd_inode` inside the kernel directly, so the packages built on them (`mkdir`, `rm`) don't need `resolve_path()`'s glue at all.

### Ctrl+C and Foreground Jobs

The keyboard driver detects Ctrl+C and signals `SIGINT` to whatever PID `SYS_GET_FOREGROUND` currently reports (see [`13_syscalls.md`](13_syscalls.md#polling-signals-alarms-4549)). Since the shell sets and clears this marker around every foreground `SYS_WAIT`, Ctrl+C reaches the running child, not the shell itself; if the child dies from it, `SYS_WAIT` simply returns like any other child exit — the shell needs no separate "was it killed by Ctrl+C" notification path.

---

## Shell Builtins

| Command | Description |
|---------|-------------|
| `cd [dir]` | Changes directory. With no argument, goes to the current user's home directory (looked up from `/etc/passwd`) rather than the root. |
| `pwd` | Prints the current working directory. |
| `exit` / `logout` | Terminates the shell process (`sys_exit(0)`) — if this is the top-level shell, it is immediately respawned by the kernel (see [`02_kernel_init.md`](02_kernel_init.md#23-create-shell-process)). |
| `help` | Prints the builtin list and a one-line pointer to the package set. |
| `history` | Lists stored command history, 1-indexed. |
| `clear` | Clears the console (`con_clear()`). |
| `echo <text...>` | Prints its arguments, space-joined. |
| `wait <pid>` | Blocks on `SYS_WAIT(pid)` for a specific (not just any) child and prints its exit code. |
| `su [user]` | Prompts for a password with local echo suppressed, then calls `SYS_SU`. Defaults to `root` if no username is given. On success, re-resolves its own `/etc/passwd` identity and `cd`s to the new user's home directory. Root needs no password for any target. |
| `mount <usb-index> <prefix>` | E.g. `mount 0 /mnt/usb0`. Wraps `SYS_MOUNT`; prints a specific message for each of `SYS_MOUNT`'s documented negative return codes (bad prefix, already mounted, no free slots, no such device, unsupported block size, device too large, out of memory, read failure, not FAT). |
| `unmount <prefix>` | Wraps `SYS_UNMOUNT`. |
| `run <program> [args...]` / `runbg <program> [args...]` / `exec <program> [args...]` | Legacy-compatible synonyms. `run`/no-prefix both block in the foreground; `runbg` (or a trailing `&`) backgrounds it; `exec` replaces the shell's own process image in place via `sys_exec()` (does not return on success) instead of forking. |

---

## Packages (`/bin/*.elf`)

Every package here is an independent dynamically-linked ELF (see [`12_elf_processes.md`](12_elf_processes.md#dynamic-linking-libcso)) installed by the package manager (see [`17_package_manager.md`](17_package_manager.md)). None of them are special-cased by the shell — they're found and run exactly like any other `/bin/*.elf`.

### Filesystem

| Command | Syntax | Description |
|---------|--------|-------------|
| `ls` | `ls [path]` | Lists directory contents. No `-l` (long format) — there is still no `stat()`-equivalent syscall to fetch permissions/owner/size for an arbitrary entry without opening it first. An executable is identified by a `.elf` suffix (no real exec-bit check available through the ABI), not by its actual permission bits. |
| `mkdir` | `mkdir <name>` | Creates a directory via `SYS_MKDIR`. Error reporting is coarser than the old kernel-native version — `SYS_MKDIR` collapses most failures into a bare `-1`. |
| `rm` | `rm <name>` or `rm *` | Removes a file or empty directory via `SYS_UNLINK`. `rm *` lists the current directory and removes every entry; the name list is heap-allocated (not a large stack array), to fit comfortably within a userspace process's 16 KiB stack. |
| `touch` | `touch <file>` | Creates an empty file via `SYS_OPEN(O_CREAT)`. Unlike the old kernel-native version, touching an existing file is **not** an error (real Unix `touch` semantics) — it's just opened. |
| `cat` | `cat <file>` | Reads the whole file into a `malloc()`'d buffer and prints it as-is — no `--- file ---` framing the old kernel-native version used to add. |
| `write` | `write <file> <text...>` | Overwrites (or creates) a file with its remaining arguments, space-joined. |
| `cp` | `cp <source> <destination>` | Copies a file by reading the source whole and writing it to the destination. No separate manual permission-check code — relies entirely on `SYS_OPEN`'s own `-EACCES`, like any ordinary Unix program would. |
| `mv` | `mv <source> <destination>` | Copies then `SYS_UNLINK`s the source. **The `rename` alias is gone** — only `mv` exists as a command name now; the old kernel-native shell accepted both names for the same logic, but nothing in `shell.elf` registers `rename` as a synonym. |

There is no `edit` (append-text) command anymore — it was never ported.

### Process Control

| Command | Syntax | Description |
|---------|--------|-------------|
| `ps` | `ps` | Lists all processes via `SYS_PSLIST` — the first command in this port that actually needed a *new* syscall, since there was previously no way at all for userspace to see any process but its own children. |
| `kill` | `kill [-SIGNAL] <pid>` | Sends a signal via `SYS_KILL`. `-SIGNAL` accepts `-term` (default), `-kill`, `-stop`, `-cont`, or a numeric signal — **lowercase only**; unlike the old kernel-native shell, `shell.elf` does not lowercase the command line before `kill.elf` sees it, so `kill -KILL 3` fails with "Unknown signal" where the kernel-native version would have accepted it. Does **not** auto-reap the target with `wait()` the way the old kernel-native command did — architecturally impossible for a separate process, since `kill.elf` (itself a child of the shell) is never the real parent of the PID it's signaling, so its own `SYS_WAIT` on that PID would just fail as "not a child." |

### Users & Permissions

See [`15_users_permissions.md`](15_users_permissions.md) for the full model.

| Command | Syntax | Description |
|---------|--------|-------------|
| `whoami` | `whoami` | Prints `uid`/`gid` plus resolved names, parsed straight out of `/etc/passwd`/`/etc/group` from userspace — the one identity command that needed no new syscall, since `SYS_GETUID`/`SYS_GETGID` already existed. |
| `chmod` | `chmod <mode> <path>` | Octal mode, via `SYS_CHMOD`. |
| `chown` | `chown <uid> <gid> <path>` | **Numeric `uid`/`gid` only** — unlike the old kernel-native `chown <user>[:group] <path>`, there is no username-to-uid resolution from userspace in this version (that would need a general "look up any name's uid" parser, not just "look up my own," which is a bigger follow-up). |
| `useradd` | `useradd <user> <password> [group]` | Via `SYS_USERADD`. Omitting `group` creates a same-named group, same as real `useradd`. |
| `groupadd` | `groupadd <group>` | Via `SYS_GROUPADD`. |
| `passwd` | `passwd <new-password>` or `passwd -u <username> <new-password>` | Via `SYS_PASSWD`. The no-`-u` form changes the caller's own password; `-u` resets any user's (root only). Password is typed as a plain, visible argument — not masked (the same deliberate simplification the old kernel-native version made; `su`'s prompt, by contrast, *is* masked — see [Shell Builtins](#shell-builtins)). |

### USB Mass Storage

| Command | Syntax | Description |
|---------|--------|-------------|
| `usbinfo` | `usbinfo` | Lists detected devices via `SYS_USB_COUNT`/`SYS_USB_INFO`. |
| `usbread` | `usbread <device> <lba>` | Reads one block via `SYS_USB_READ`, hex-dumped. |
| `usbwrite` | `usbwrite <device> <lba> <text>` | Writes text into one block via `SYS_USB_WRITE` (root only). |

`mount`/`unmount` themselves are shell builtins (see [Shell Builtins](#shell-builtins)), not packages, since they can't be split cleanly from the `SYS_MOUNT`/`SYS_UNMOUNT` call either way — there was no strong reason to make them one or the other, and they ended up as builtins.

### Color

| Command | Syntax | Description |
|---------|--------|-------------|
| `color` | `color <fg> [bg]` or `color reset` | Sets foreground (and optionally background) using a one-byte palette index, hex `00`–`FF`. |
| `fg` / `bg` | `fg <color>` / `bg <color>` | Sets just one half of the pair. |
| `reset` | `reset` | Resets to white-on-black. |

**The 6-digit RGB hex mode from the old kernel-native `color` command is gone.** `/dev/console`'s write-side escape protocol (`userspace/common/console.h`) only carries a color as a single byte (`con_set_fg()`/`con_set_bg()`) — there is no syscall-level primitive for an arbitrary 24-bit RGB color, so only the palette-index form survived the port. There is also no `colors` (plural) command to print the palette table anymore.

### System / Power

| Command | Syntax | Description |
|---------|--------|-------------|
| `devmode` | `devmode <on\|off>` | Via `SYS_DEVMODE`. Unlike the old kernel-native version, there is no no-argument "show current state" form in this port — an argument is required. |
| `reboot` | `reboot` | Via `SYS_REBOOT` (root only; flushes LufiraFS first, inside the kernel). |
| `shutdown` | `shutdown` | Via `SYS_SHUTDOWN` (root only; same flush-first behavior). |

**`version`, `trap`, and `status` are gone.** None of the old kernel-native diagnostic commands (`version` — kernel build info; `trap` — deliberately trigger a CPU exception; `status` — interrupt/CPU state) were ported; there is no equivalent package and no shell builtin for any of them.

### Package Management

| Command | Syntax | Description |
|---------|--------|-------------|
| `dlpg` | `dlpg install\|update\|list\|remove\|sync\|upgrade ...` | The package manager itself — see [`17_package_manager.md`](17_package_manager.md) for the full command set. |

Also in `lufira-packages/user/`, documented together since they share one pattern (read a fixed-size stats struct via one syscall, print it):

| Command | Syscall | Description |
|---------|---------|-------------|
| `df` | `SYS_STATFS` | LufiraFS free/used blocks and inodes. |
| `du [path]` | `SYS_OPEN`/`SYS_SEEK`/`SYS_READDIR` (no dedicated syscall) | Recursive disk usage; prints every nested file/directory at its own level (a deliberately different, more verbose shape than the old kernel-native version's immediate-children-plus-total output — both are honest about what they show, just different). |
| `free` | `SYS_MEMINFO` | Physical memory and kernel heap totals. |
| `cpuload` | `SYS_CPULOAD` + `SYS_PSLIST` | Two tick-count samples with a `sleep()` between them, used to compute a system-wide busy percentage and (since `SYS_PSLIST` exists) a per-process breakdown. |

---

## Command Reference

| Command | Syntax | Where |
|---------|--------|-------|
| `cd` | `cd [dir]` | shell builtin |
| `pwd` | `pwd` | shell builtin |
| `exit` / `logout` | `exit` | shell builtin |
| `help` | `help` | shell builtin |
| `history` | `history` | shell builtin |
| `clear` | `clear` | shell builtin |
| `echo` | `echo <text...>` | shell builtin |
| `wait` | `wait <pid>` | shell builtin |
| `su` | `su [user]` | shell builtin |
| `mount` | `mount <usb-index> <prefix>` | shell builtin |
| `unmount` | `unmount <prefix>` | shell builtin |
| `run` / `runbg` / `exec` | `run <program> [args...]` | shell builtin (external-program dispatch) |
| `ls` | `ls [path]` | `/bin/ls.elf` |
| `mkdir` | `mkdir <name>` | `/bin/mkdir.elf` |
| `rm` | `rm <name>` or `rm *` | `/bin/rm.elf` |
| `touch` | `touch <file>` | `/bin/touch.elf` |
| `cat` | `cat <file>` | `/bin/cat.elf` |
| `write` | `write <file> <text...>` | `/bin/write.elf` |
| `cp` | `cp <source> <destination>` | `/bin/cp.elf` |
| `mv` | `mv <source> <destination>` | `/bin/mv.elf` |
| `ps` | `ps` | `/bin/ps.elf` |
| `kill` | `kill [-SIGNAL] <pid>` | `/bin/kill.elf` |
| `whoami` | `whoami` | `/bin/whoami.elf` |
| `chmod` | `chmod <mode> <path>` | `/bin/chmod.elf` |
| `chown` | `chown <uid> <gid> <path>` | `/bin/chown.elf` |
| `useradd` | `useradd <user> <password> [group]` | `/bin/useradd.elf` |
| `groupadd` | `groupadd <group>` | `/bin/groupadd.elf` |
| `passwd` | `passwd <new-password>` / `passwd -u <user> <new-password>` | `/bin/passwd.elf` |
| `usbinfo` | `usbinfo` | `/bin/usbinfo.elf` |
| `usbread` | `usbread <device> <lba>` | `/bin/usbread.elf` |
| `usbwrite` | `usbwrite <device> <lba> <text>` | `/bin/usbwrite.elf` |
| `color` | `color <fg> [bg]` / `color reset` | `/bin/color.elf` |
| `fg` | `fg <color>` | `/bin/fg.elf` |
| `bg` | `bg <color>` | `/bin/bg.elf` |
| `reset` | `reset` | `/bin/reset.elf` |
| `devmode` | `devmode <on\|off>` | `/bin/devmode.elf` |
| `reboot` | `reboot` | `/bin/reboot.elf` |
| `shutdown` | `shutdown` | `/bin/shutdown.elf` |
| `dlpg` | `dlpg <subcommand> ...` | `/bin/dlpg.elf` — see [`17_package_manager.md`](17_package_manager.md) |
| `df` | `df` | `/bin/df.elf` |
| `du` | `du [path]` | `/bin/du.elf` |
| `free` | `free` | `/bin/free.elf` |
| `cpuload` | `cpuload` | `/bin/cpuload.elf` |

---

## What Didn't Make the Move

These were kernel-native commands before v0.7 and have **no userspace equivalent at all** — not a syscall, not a package, not a shell builtin. Typing any of them just gets "Unknown command" (the shell's generic response to any name it can't resolve under `/bin/`):

| Removed command(s) | Why it's gone |
|---------------------|----------------|
| `ifconfig`, `ping`, `wget` | Networking (see [`16_networking.md`](16_networking.md)) never got a general-purpose syscall for raw sockets/ICMP/interface configuration — only `SYS_NET_FETCH` exists, a single blocking "fetch this URL" call, and its only caller today is `dlpg sync`/`upgrade` (see [`17_package_manager.md`](17_package_manager.md)). There's no package that exposes `SYS_NET_FETCH` as a general-purpose command. |
| `beep`, `mixer`, `music` | Audio (`kernel/drivers/sound/ac97.c`) has no syscall surface at all — the AC'97 driver is still kernel-internal-only, and nothing exposes it to userspace. |
| `version`, `trap`, `status` | Diagnostic/demo commands with no clear userspace replacement priority; simply not ported. |
| `edit` | Append-to-file; superseded in spirit by `write` (full overwrite) but never itself ported. |
| `colors` (plural) | The palette-table display command; `color`/`fg`/`bg`/`reset` (the ones that actually *change* color) were ported, this purely informational one was not. |
| `rename` | Not a separate command anymore — `mv` absorbed its logic but the old shell's second name for the same command didn't carry over. |

The shell's own source comment states this plainly: *"Network/sound still hit kernel-internal functions without a syscall — not ported to packages or here; typing such a command honestly answers 'unknown command,' like any other not-found program."*

---

## Integration with Kernel Components

### Keyboard Driver

`shell.elf` never talks to the keyboard driver directly — it just blocks on `sys_read(0, &c, 1)` against `/dev/console`'s input ring buffer, which `drivers/input/input.c` fills from both the PS/2 and USB HID keyboard paths (see [`07_drivers.md`](07_drivers.md)). Ctrl+Up/Down (console scrollback) and Ctrl+C (foreground `SIGINT`) are handled by the kernel itself before a byte would even reach that ring buffer — see [Ctrl+C and Foreground Jobs](#ctrlc-and-foreground-jobs).

### Filesystem (VFS/LufiraFS)

Every filesystem package goes through the ordinary syscall ABI (`SYS_OPEN`/`SYS_READ`/`SYS_WRITE`/`SYS_MKDIR`/`SYS_RMDIR`/`SYS_UNLINK`/`SYS_READDIR`/`SYS_CHDIR`) — see [`13_syscalls.md`](13_syscalls.md) for exact semantics and [`08_filesystem.md`](08_filesystem.md) for what happens inside the kernel on each call. There is no separate "shell filesystem command" code path anymore distinct from what any other program would do to open/read/write a file.

### Process Manager and ELF Loader

`run`/`runbg`/background-`&`/plain external-command dispatch all go through `SYS_FORK` + `SYS_EXEC` + `SYS_WAIT` (`shell.elf`'s own `run_child()`/`dispatch()`) — see [`12_elf_processes.md`](12_elf_processes.md). `ps`/`kill`/`wait` are thin wrappers over `SYS_PSLIST`/`SYS_KILL`/`SYS_WAIT` respectively, same as any userspace program calling them would be.

### Users and Permissions

See [`15_users_permissions.md`](15_users_permissions.md) for the full on-disk format and permission model, and its [Shell Commands](15_users_permissions.md#shell-commands) section specifically for how each identity command now maps to a syscall.

### USB Mass Storage and FAT

`usbinfo`/`usbread`/`usbwrite` wrap `SYS_USB_COUNT`/`SYS_USB_INFO`/`SYS_USB_READ`/`SYS_USB_WRITE` directly. `mount`/`unmount` wrap `SYS_MOUNT`/`SYS_UNMOUNT`, which — unlike the pre-v0.7 kernel-native `mount` command this replaced — really do integrate with the VFS: once mounted, ordinary `open`/`read`/`write`/`mkdir`/`unlink`/`readdir` (and therefore `cat`/`cp`/`ls`/`mkdir`/`rm`) see the mounted device's root directory transparently under the given prefix, with real-time write-back after every write, not just on `unmount`. See [`08_filesystem.md`](08_filesystem.md#fat-and-usb-mass-storage-mounting).

### Developer Mode and Logging

`devmode.elf` wraps `SYS_DEVMODE`, a thin syscall over `devmode_set()`/`devmode_is_enabled()` (`system/devmode/`) — see [`04_logging.md`](04_logging.md) for the full design. The kernel-side logging behavior itself is unchanged by any of this; only how userspace flips the switch changed.

---

## Conclusion

The interactive shell and almost all of the command set it drives moved out of the kernel binary entirely in v0.7 — `shell.elf` is an ordinary preemptible userspace process, and the roughly forty commands it can run are independent, dynamically-linked packages in a separate repository, installed and updated through the package manager described in [`17_package_manager.md`](17_package_manager.md). The syscall ABI (SYS_OPEN/SYS_MKDIR/SYS_PSLIST/SYS_SU/…, see [`13_syscalls.md`](13_syscalls.md)) is what makes this possible: every one of these packages does exactly what the old kernel-native command used to do, just through a syscall instead of a direct function call inside the kernel. A handful of commands — mostly ones needing a syscall surface that doesn't exist yet (networking, audio) — didn't make the move and are simply gone; see [What Didn't Make the Move](#what-didnt-make-the-move).

For more details, refer to the source code in this repository's `kernel/system/syscall/syscall.c` (for what each syscall actually does) and the sibling `lufira-packages` repository's `shell/shell.c`, `base/`, and `user/` (for what each command actually does with it).

---

**Document Version:** 3.0
**Last Updated:** October 2026
**Project:** LufiraOS
