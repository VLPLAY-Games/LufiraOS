# Shell and Built-in Commands

This document describes the interactive shell and the built-in command set of LufiraOS. The shell provides a command-line interface for system control, file management, process control, and audio playback.

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
   - [Audio Commands](#audio-commands)
4. [Command Reference](#command-reference)
5. [Integration with Kernel Components](#integration-with-kernel-components)
   - [Keyboard Driver](#keyboard-driver)
   - [Filesystem (FAT/VFS)](#filesystem-fatvfs)
   - [Process Manager and ELF Loader](#process-manager-and-elf-loader)
   - [Audio (AC'97)](#audio-ac97)
   - [ACPI](#acpi)
6. [Future Extensions](#future-extensions)

---

## Overview

The LufiraOS shell is a simple, interactive command interpreter that runs as a user process. It provides:

- **Command-line interface** – users type commands and see output.
- **Line editing** – insert/delete characters, cursor movement.
- **Command history** – up to 20 commands stored, navigation with arrow keys.
- **Tab completion** – auto-completes command names.
- **Current working directory** – persistent state for filesystem commands.

**Design Philosophy:**
- **Simplicity** – the shell is easy to understand and extend.
- **Responsiveness** – it reacts quickly to user input.
- **Integration** – it leverages all kernel subsystems.

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

**Command List:** Built-in commands: help, clear, reboot, shutdown, version, echo, history, status, trap, color, colors, fg, bg, reset, pwd, cd, ls, mkdir, rm, touch, cat, run, runbg, write, beep, mixer, music, ps, kill.

### Current Working Directory

The shell maintains two global variables:

| Variable | Description |
|----------|-------------|
| `cwd_path[256]` | The current directory path as a string (e.g., `/home/user`). |
| `cwd_first_cluster` | The FAT cluster number of the current directory (`0` = root). |

**Commands that use CWD:**
- `pwd` – prints the current path.
- `cd` – changes the current directory.
- `ls` – lists files in the current directory.
- `mkdir` – creates a directory in the current directory.
- `rm` – removes a file in the current directory.
- `touch` – creates a file in the current directory.
- `cat` – reads a file from the current directory.

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
| `ls [-l]` | Lists directory contents. `-l` shows long format (type, size, name). |
| `mkdir <name>` | Creates a new directory. |
| `rm <name>` or `rm *` | Removes a file or empty directory. `rm *` removes all items. |
| `touch <filename>` | Creates an empty file. |
| `cat <file>` | Displays the contents of a file. |
| `write <file> <text>` | Writes text to a file (overwrites if exists). |
| `edit <file> <text>` | Appends text (with newline) to a file. |
| `cp <src> <dst>` | Copies a file. |
| `mv <src> <dst>` | Moves/renames a file. |
| `rename <old> <new>` | Renames a file (alias for `mv`). |

### Process Commands

| Command | Description |
|---------|-------------|
| `run <filename>` | Loads an ELF file and runs it in the foreground (blocks the shell). |
| `runbg <filename>` | Loads an ELF file and runs it in the background (shell returns immediately). |
| `ps` | Lists running processes with PID, state, and name. |
| `kill <pid>` | Terminates a process by PID. |

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
| `reboot` | `reboot` | Flushes FAT and reboots. |
| `shutdown` | `shutdown` | Flushes FAT and shuts down. |
| `version` | `version` | Shows kernel version. |
| `status` | `status` | Shows interrupt and CPU status. |
| `trap` | `trap <type>` | Triggers exception (`int3`, `ud2`, `pf`, `cli`, `sti`, `hlt`). |
| `echo` | `echo <text>` | Prints the argument. |
| `history` | `history` | Shows command history. |
| `color` | `color <fg> [bg]` | Sets colours (hex index or RGB). |
| `colors` | `colors` | Shows colour palette. |
| `fg` | `fg <color>` | Sets foreground only. |
| `bg` | `bg <color>` | Sets background only. |
| `reset` | `reset` | Resets to default colours. |
| `pwd` | `pwd` | Shows current directory. |
| `cd` | `cd <dir>` | Changes directory (supports `..`). |
| `ls` | `ls [-l]` | Lists directory entries. |
| `mkdir` | `mkdir <name>` | Creates a directory. |
| `rm` | `rm <name>` or `rm *` | Removes a file/directory (or all). |
| `touch` | `touch <filename>` | Creates an empty file. |
| `cat` | `cat <filename>` | Displays file content. |
| `write` | `write <file> <text>` | Writes text to a file. |
| `edit` | `edit <file> <text>` | Appends text (with newline). |
| `cp` | `cp <src> <dst>` | Copies a file. |
| `mv` | `mv <src> <dst>` | Moves/renames a file. |
| `rename` | `rename <old> <new>` | Renames a file. |
| `run` | `run <filename>` | Executes an ELF program. |
| `runbg` | `runbg <filename>` | Executes an ELF program in background. |
| `ps` | `ps` | Lists running processes. |
| `kill` | `kill <pid>` | Terminates a process. |
| `beep` | `beep` | Plays a beep. |
| `mixer` | `mixer [0-100]` | Gets/sets audio volume. |
| `music` | `music` | Plays a test melody. |

---

## Integration with Kernel Components

### Keyboard Driver

The shell receives keyboard input through the keyboard driver:

1. Keyboard interrupt (IRQ1) → `keyboard_irq_handler()`.
2. Scancode → ASCII conversion → `process_keypress()`.
3. Shell handlers: `shell_handle_char()`, `shell_handle_backspace()`, etc.

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

### Filesystem (FAT/VFS)

Filesystem commands use the FAT driver and VFS:

- `ls` – uses `fat_opendir()` and `fat_readdir()` to list directory entries.
- `cd` – uses `fat_find_entry()` to locate a directory and updates CWD.
- `mkdir` – calls `fat_mkdir()`.
- `rm` – calls `fat_rm()`.
- `touch` – calls `fat_create_file()`.
- `cat` – calls `fat_open()` and `fat_read_file()`.
- `write` – calls `fat_write_file()`.
- `edit` – calls `fat_append_file()`.
- `cp` – reads the source file and writes to the destination.
- `mv` – copies the file and deletes the source.

### Process Manager and ELF Loader

Process commands use the process manager and ELF loader:

- `run` – calls `elf_exec()` (foreground, blocks shell).
- `runbg` – calls `elf_exec_background()` (background, returns immediately).
- `ps` – calls `process_ps()` to list all processes.
- `kill` – calls `process_kill()` to terminate a process.

### Audio (AC'97)

Audio commands use the AC'97 driver:

- `beep` – calls `ac97_play_tone(440, 250)`.
- `mixer` – calls `ac97_set_volume()` and `ac97_get_volume()`.
- `music` – plays a predefined melody using `ac97_play_tone()`.

### ACPI

The `shutdown` command uses ACPI:

- `acpi_shutdown()` – performs ACPI S5 shutdown.
- Falls back to legacy ports if ACPI is not available.

---

## Conclusion

The LufiraOS shell provides a functional command-line environment with a rich set of built-in commands. Its tight integration with the console, keyboard, filesystem, process manager, and audio subsystems makes it a powerful tool for system control, development, and debugging. The simple, modular design allows for easy extension with new commands and features.

For more details, refer to the source code in `shell/` and `shell/commands/`.

---

**Document Version:** 1.0  
**Last Updated:** September 2026  
**Project:** LufiraOS