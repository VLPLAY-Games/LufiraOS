# Users, Groups & Permissions

This document describes LufiraOS's Unix-like users/groups/permissions subsystem: the on-disk `uid`/`gid`/`perm` fields carried by every LufiraFS inode, the owner/group/other permission check, the flat-file user/group database (`/etc/passwd`, `/etc/group`), how process identity is established and propagated, and the shell commands and syscalls that expose all of this to userspace.

---

## Table of Contents

1. [Overview](#overview)
2. [On-Disk Inode Format](#on-disk-inode-format)
3. [Permission Checking](#permission-checking)
   - [Bit Selection Algorithm](#bit-selection-algorithm)
   - [Enforcement Coverage](#enforcement-coverage)
4. [User and Group Database](#user-and-group-database)
   - [Database Structures](#database-structures)
   - [Passwd and Group File Format](#passwd-and-group-file-format)
   - [Password Hashing](#password-hashing)
   - [Lookup and Add API](#lookup-and-add-api)
   - [Default and Seeded Accounts](#default-and-seeded-accounts)
5. [Process Identity](#process-identity)
6. [New Syscalls](#new-syscalls)
7. [Shell Commands](#shell-commands)
8. [Known Limitations](#known-limitations)
9. [Conclusion](#conclusion)

---

## Overview

Every LufiraFS inode now carries a `uid`, a `gid`, and a classic 9-bit `rwxrwxrwx` `perm` field. Every user has one numeric `uid` and exactly one primary `gid` (no multi-group membership), looked up against a small flat-file database (`/etc/passwd`, `/etc/group`) parsed once at boot into fixed-size in-memory tables. Permission checks compare a process's `uid`/`gid` against an inode's owner/group/other bits — except that `uid == 0` (`root`) unconditionally bypasses every check, exactly like real Unix. Accounts are persistent: `useradd`/`groupadd` append to the on-disk passwd/group files immediately, so new users survive a reboot. There is no login prompt anywhere in the boot sequence — the kernel boots straight into a shell process that inherits `uid=0`/`gid=0` from the kernel's idle process, i.e. every session starts as `root` until something (`su`) changes it.

**Design Philosophy:**
- **Minimalism** – one primary group per user, no ACLs, no setuid/setgid bit, no `euid`/`egid` distinction (a process's "real" and "effective" identity are always the same value).
- **Familiarity** – the on-disk permission model, the `chmod`/`chown` octal-mode conventions, and the `passwd`/`group` colon-separated text format all deliberately mirror early Unix, making the subsystem easy to reason about despite its small size.
- **No login flow** – LufiraOS is a single-machine hobby kernel; the very first shell process runs as `root` by construction (see [Process Identity](#process-identity)), and `su` is the only way to drop privileges.

---

## On-Disk Inode Format

`kernel/fs/lufirafs/lufirafs_format.h` defines `LUFIRAFS_VERSION` as `2`, with the header's own comment noting the reason: `// v2: inode incl. uid/gid/perm`. The inode grew from its original layout to add three new 4-byte fields:

```c
// mode + size + links_count + direct[12] + indirect + uid + gid + perm
// = 4+4+4+48+4+4+4+4 = 76 bytes.
typedef struct {
    uint32_t mode;             // LUFIRAFS_MODE_*
    uint32_t size;              // bytes (for directories, bytes of dirent data)
    uint32_t links_count;       // >=1 while live; 0 = free
    uint32_t direct[LUFIRAFS_DIRECT_BLOCKS]; // 12 direct block pointers
    uint32_t indirect;          // 0 = no indirect block
    uint32_t uid;                // owner (0 = root)
    uint32_t gid;                // owning group
    uint32_t perm;                // classic 9-bit rwxrwxrwx (e.g. 0644/0755)
} __attribute__((packed)) lufirafs_inode_t;
```

| Field | Type | Description |
|-------|------|-------------|
| `mode` | `uint32_t` | `LUFIRAFS_MODE_FREE`/`FILE`/`DIR`. |
| `size` | `uint32_t` | Size in bytes. |
| `links_count` | `uint32_t` | Live/free marker. |
| `direct[12]` | `uint32_t[12]` | Direct block pointers. |
| `indirect` | `uint32_t` | Single-indirect block pointer. |
| `uid` | `uint32_t` | Owning user ID. |
| `gid` | `uint32_t` | Owning group ID. |
| `perm` | `uint32_t` | 9-bit `rwxrwxrwx` permission mode. |

`LUFIRAFS_INODE_SIZE` is `76` bytes — up from the pre-`uid`/`gid`/`perm` layout — and `LUFIRAFS_INODE_COUNT` (512 inodes) is unchanged, so the inode table simply occupies more blocks for the same inode count. New files/directories get `LUFIRAFS_DEFAULT_FILE_PERM` (`0644`) or `LUFIRAFS_DEFAULT_DIR_PERM` (`0755`) respectively, unless a caller (e.g. `useradd`'s home-directory creation, see [Default and Seeded Accounts](#default-and-seeded-accounts)) passes an explicit mode.

Growing the inode by 12 bytes is a breaking on-disk format change — a `disk.img` formatted by the old `mkfs_lufirafs`/kernel pair would misparse every inode under the new 76-byte stride. This is acceptable here (and is, in effect, the same reasoning the surrounding build already relies on for the format header — both the freestanding kernel driver and the hosted `tools/mkfs_lufirafs.c` include the exact same `lufirafs_format.h`, so the two can never silently disagree) because `disk.img` is a build artifact: the sibling `LufiraOS-Builder` repository's `build` step (see [`05_build_system.md`](05_build_system.md)) always runs `mkfs_lufirafs format` from scratch before staging `/etc/passwd`/`/etc/group` and everything else, for every clean build. There is no persistent installed system and therefore no migration path to maintain.

---

## Permission Checking

### Bit Selection Algorithm

`lufirafs_check_access()` (`kernel/fs/lufirafs/lufirafs.c`) is the single function that decides whether an operation is allowed:

```c
int lufirafs_check_access(const lufirafs_inode_t *inode, uint32_t uid, uint32_t gid,
                           int want_read, int want_write, int want_exec) {
    if (!inode) return 0;
    if (uid == 0) return 1; // root bypasses every check

    uint32_t bits;
    if (inode->uid == uid) bits = (inode->perm >> 6) & 07u;      // owner bits
    else if (inode->gid == gid) bits = (inode->perm >> 3) & 07u; // group bits
    else bits = inode->perm & 07u;                                // other bits

    if (want_read  && !(bits & 04u)) return 0;
    if (want_write && !(bits & 02u)) return 0;
    if (want_exec  && !(bits & 01u)) return 0;
    return 1;
}
```

The rule set, in order:

1. **Root bypass** — `uid == 0` always returns `1` (allowed), before any bit is even read. There is no way to restrict root.
2. **Owner match** — if the caller's `uid` equals the inode's `uid`, the top 3 bits (`perm >> 6`) apply.
3. **Group match** — otherwise, if the caller's `gid` equals the inode's `gid`, the middle 3 bits (`perm >> 3`) apply.
4. **Other** — otherwise, the bottom 3 bits apply.
5. Each requested capability (`want_read`/`want_write`/`want_exec`) is checked independently against the selected 3-bit group; any missing requested bit fails the whole check.

### Enforcement Coverage

As of v0.7, the interactive shell is a userspace ELF (`lufira-packages/shell/shell.c`) and every file-manipulating command (`cp`/`mv`/`ls`/`mkdir`/`rm`/`cat`/`touch`/`write`/…) is its own small package under `lufira-packages/base/`, none of which can touch LufiraFS directly — they all go through the syscall ABI like any other userspace program. The old `check_perm()` helper in `kernel/shell/commands/filesystem.c` belonged to the kernel-native shell and is now dead code, never called. **All enforcement today lives in `kernel/system/syscall/syscall.c`** (plus `do_exec()`), which means it applies uniformly to every caller — a package, the shell's own builtins, or anything else — rather than being re-implemented (and potentially missed) per command:

| Operation | Where | Checks |
|-----------|-------|--------|
| `SYS_OPEN` (`open()` — backs `cat`/`cp`/`mv`/`touch`/`write`/`ls`/…) | `syscall.c` | read and/or write on the target (depending on `O_RDONLY`/`O_WRONLY`/`O_RDWR`), or write+exec on the parent directory when `O_CREAT` creates a new entry |
| `SYS_MKDIR` (`mkdir`) | `syscall.c` | write+exec on the parent (resolved relative to `cwd_inode`) |
| `SYS_RMDIR` / `SYS_UNLINK` (`rm`) | `syscall.c` (`sys_remove()`) | write+exec on the parent |
| `exec` / `run` / `SYS_EXEC` | `do_exec()` | exec bit on the target file |
| `SYS_CHMOD` / `SYS_CHOWN` | `syscall.c` | owner-or-root (`chmod`), root-only (`chown`) — see [New Syscalls](#new-syscalls) |

Because every package that touches a file does so by calling `open()` (with `O_RDONLY`/`O_WRONLY`/`O_CREAT` as appropriate), the net effect reproduces the old per-command checks without duplicating them: `cp` ends up read-checked on its source (plain `open(O_RDONLY)`) and write/parent-checked on its destination (`open(O_CREAT|O_WRONLY)`), `cat` is read-checked, `touch`/`write` are write/parent-checked, and so on — just via one shared code path instead of one `check_perm()` call per command.

One behavior actually **improved** as a side effect of this move: the old kernel-native `ls` (`command_ls()`) called `lufirafs_opendir()`/`lufirafs_readdir()` directly, bypassing permission checks entirely. The new userspace `ls` (`lufira-packages/base/ls.c`) opens the target directory with a plain `sys_open(path, O_RDONLY, 0)` first — which **does** go through `SYS_OPEN`'s read-permission check above — so listing a directory now actually requires read permission on it, matching real Unix semantics (where listing needs read, not exec, on the directory).

One gap remains exactly as before: **`cd` still does not check anything.** `SYS_CHDIR` (`sys_chdir()` in `syscall.c`) resolves the target via `lufirafs_lookup()` and never calls `lufirafs_check_access()` at all — changing into a directory still does not check its exec ("search") bit, regardless of whether the caller is the shell's own `cd` builtin or something else calling `SYS_CHDIR` directly (see [Known Limitations](#known-limitations)).

---

## User and Group Database

### Database Structures

`kernel/system/users/users.h` defines fixed-size tables, parsed once at boot (`users_init()`, called from `kernel.c` right after `lufirafs_init()`/`devmode_init()`/`klog_init()`, before the shell process is created):

```c
#define MAX_USERS  32
#define MAX_GROUPS 32

typedef struct {
    char username[32];
    uint32_t uid;
    uint32_t gid;
    uint32_t password_hash;   // FNV-1a of the password
    char home[64];
    int in_use;
} user_entry_t;

typedef struct {
    char groupname[32];
    uint32_t gid;
    int in_use;
} group_entry_t;
```

Both tables (`g_users[MAX_USERS]`, `g_groups[MAX_GROUPS]`) are static, fixed-capacity arrays — the same pattern as `MAX_PROCESSES`/`MAX_MMAP_REGIONS` elsewhere in the kernel. There is a hard ceiling of 32 users and 32 groups; `users_add()`/`groups_add()` simply fail (`-1`) once every slot is `in_use`.

If `/etc/passwd`/`/etc/group` are missing or unreadable (a fresh or corrupted disk), `users_init()` leaves both tables empty — `whoami`/`su` will then find nobody, and every uid/gid the running shell reports resolves to "unknown" rather than crashing.

### Passwd and Group File Format

Both files are plain colon-separated text, parsed by mutating the line buffer in place (`split_fields()` replaces `:` with `\0`) since this freestanding environment has no `strtok`.

**`/etc/passwd`** — `username:uid:gid:password_hash_hex:home` (the trailing `home` field is optional; if absent, `home` defaults to `/`):

```
root:0:0:3774e5d9:/
guest:1000:1000:811c9dc5:/
```

**`/etc/group`** — `groupname:gid`:

```
root:0
users:1000
```

Both examples above are taken verbatim from the seeded `tools/seed/passwd`/`tools/seed/group` (see [Default and Seeded Accounts](#default-and-seeded-accounts)). Lines starting with `#` and blank lines are skipped. Parsing stops early if it hits `MAX_USERS`/`MAX_GROUPS` entries.

### Password Hashing

Passwords are hashed with **FNV-1a, 32-bit**, stored as 8 hex digits in the passwd file's 4th field:

```c
// FNV-1a 32-bit — a simple non-cryptographic hash; nothing stronger is
// needed here (hobby OS, no crypto library present at all).
static uint32_t fnv1a_hash(const char *s) {
    uint32_t h = 0x811c9dc5u;
    while (*s) {
        h ^= (uint8_t)(*s++);
        h *= 0x01000193u;
    }
    return h;
}
```

This is **not** a cryptographically secure hash and should not be treated as one:

- No salt — identical passwords across accounts (or a precomputed FNV-1a rainbow table) produce identical hashes, immediately visible to anyone who can read `/etc/passwd`.
- A 32-bit output space is small enough to brute-force or reverse offline in well under a second on modern hardware.
- FNV-1a was designed for hash-table distribution, not for resisting deliberate collision/preimage search.
- `/etc/passwd` itself is created with `uid=0`, `gid=0`, `perm=0644` (`LUFIRAFS_DEFAULT_FILE_PERM`, set by `tools/mkfs_lufirafs.c` when it `put`s the seed file) — i.e. it is world-readable by design, so any non-root user can read every stored hash directly, not just root.

`users_check_password(name, password)` returns `1` only if `fnv1a_hash(password ? password : "")` equals the stored hash for that username, `0` otherwise (including "user not found").

### Lookup and Add API

```c
void users_init(void);

int users_lookup_by_name(const char *name, user_entry_t *out);
int users_lookup_by_uid(uint32_t uid, user_entry_t *out);
int groups_lookup_by_name(const char *name, group_entry_t *out);
int groups_lookup_by_gid(uint32_t gid, group_entry_t *out);

int users_check_password(const char *name, const char *password);

int users_add(const char *name, uint32_t uid, uint32_t gid, const char *password, const char *home);
int groups_add(const char *name, uint32_t gid);

uint32_t users_next_free_uid(void);
uint32_t groups_next_free_gid(void);
```

All four lookup functions return `0` on success (with `*out` filled in when non-NULL) or `-1` if nothing matched — a plain linear scan over the in-memory array (`in_use` entries only).

`users_add()`/`groups_add()` do two things atomically from the caller's point of view: they append a freshly formatted line (`"name:uid:gid:hash:home\n"` / `"name:gid\n"`) to the end of the on-disk file via `lufirafs_write()` + `lufirafs_sync()`, *and* they write directly into the next free in-memory slot — so a newly created account is usable immediately, with no re-parse of the file and no reboot required. Both refuse to add a duplicate name (checked via the corresponding `*_lookup_by_name()` first).

`users_next_free_uid()`/`groups_next_free_gid()` start scanning at **1000** and return the first id with no matching entry — ids `0`–`999` are treated as reserved for system accounts (`root = 0` being the only one actually seeded).

### Default and Seeded Accounts

`tools/seed/passwd` and `tools/seed/group` are copied into `/etc/passwd`/`/etc/group` on every image build. This staging step no longer happens in this repository's own `Makefile` (which, since the build split described in [`05_build_system.md`](05_build_system.md), only builds `kernel.bin`/`BOOTX64.EFI` and does not touch `disk.img` at all) — it's done by the sibling `LufiraOS-Builder` repository's `build.py` (`lufira_builder/image.py`, via `mkfs_lufirafs put` against this repo's `tools/seed/passwd`/`tools/seed/group`). Their contents ship exactly two accounts:

| Account | uid | gid | Password | Notes |
|---------|-----|-----|----------|-------|
| `root` | 0 | 0 | `toor` (FNV-1a hash `3774e5d9`) | Full-privilege account; also the identity every shell starts as by default (see [Process Identity](#process-identity)). |
| `guest` | 1000 | 1000 (`users` group) | *empty string* (hash `811c9dc5`, which is `fnv1a_hash("")`) | A demo/unprivileged account with no password at all — `su guest` from a non-root shell succeeds with no password argument. |

Both are real security caveats if this system is ever exposed beyond a local, single-user hobby context: the root password is a well-known hardcoded default (`toor`, the classic "root spelled backwards" convention) baked into every built image, and the seeded `guest` account requires no password whatsoever. Neither is rotated or prompted for at first boot — there is no first-boot setup flow.

---

## Process Identity

`process_t` (`kernel/system/process/process.h`) carries identity as two plain fields, with no `euid`/`egid` split since there is no setuid mechanism to make them diverge:

```c
uint32_t uid;
uint32_t gid;
```

Propagation across the three ways a process comes into existence:

| Path | Behaviour |
|------|-----------|
| `process_create()` | `proc->uid = current_process ? current_process->uid : 0;` (same for `gid`) — a freshly created process inherits identity from whichever process called `process_create()`, exactly like `cwd_inode` does **not** do (that's always reset to the root). This is what makes `run`/`runbg` launch as the invoking shell's own user rather than always as root. |
| `process_fork()` (`SYS_FORK`) | `child->uid = parent->uid; child->gid = parent->gid;` — a straight 1:1 copy, matching POSIX `fork()`. |
| `process_commit_exec()` (`SYS_EXEC` / shell `exec`) | Identity fields are **not touched at all**. The source comment is explicit: `// uid/gid тоже сознательно НЕ трогаются — POSIX execve() сохраняет identity процесса, кроме случая setuid-бита на исполняемом файле, которого в этой минимальной реализации нет вообще` ("uid/gid are deliberately left untouched — POSIX `execve()` preserves process identity except for a setuid bit on the executable, which does not exist at all in this minimal implementation"). This matches real `execve()` semantics with the explicit caveat that **there is no setuid bit** — a non-root user can never gain elevated privilege by running a particular executable. |

**Boot-time default identity:** the kernel's `idle_process` is constructed directly (not via `process_create()`, since it's `kmalloc`'d by hand before the process subsystem is otherwise ready) and has `idle_process->uid = 0; idle_process->gid = 0;` set explicitly, with the source comment calling it out as the system's identity "point zero": *"idle is the identity starting point for the whole system (the first shell inherits from it via `process_create()`), so it must be root, not kmalloc garbage."* When `kernel.c` later calls `spawn_shell_process()` (which itself calls `process_create("shell", NULL)` and loads `/bin/shell.elf` into it — see [`02_kernel_init.md`](02_kernel_init.md#23-create-shell-process)), `current_process` at that point is still `idle_process` (uid 0), so the very first shell process inherits `uid=0`/`gid=0` — LufiraOS boots straight into a root shell, with no login prompt anywhere in the sequence. The only way to drop privilege afterward is the `su` builtin (now in `lufira-packages/shell/shell.c`, backed by the `SYS_SU` syscall) mutating `current_process->uid`/`gid` in place (see [Shell Commands](#shell-commands)).

---

## New Syscalls

Eight syscalls expose identity/permission operations to userspace. Full signatures, return values, and error codes are documented in [13_syscalls.md](13_syscalls.md) — this table is a summary only.

| # | Name | Description |
|---|------|-------------|
| 19 | `SYS_CHMOD` | Sets a file's 9-bit permission mode; owner or root only. |
| 20 | `SYS_CHOWN` | Changes a file's owner and group; root only. |
| 21 | `SYS_GETUID` | Returns the calling process's `uid`. |
| 22 | `SYS_GETGID` | Returns the calling process's `gid`. |
| 32 | `SYS_SU` | Checks `username`/`password` against `users_check_password()` **inside the kernel** (userspace is not trusted to call this only after its own check) and, on success, mutates the *calling* process's own `uid`/`gid` in place — root may switch to anyone with no password. |
| 38 | `SYS_USERADD` | Root-only. Thin wrapper over `users_add()`/`groups_add()`, plus home-directory creation. |
| 39 | `SYS_GROUPADD` | Root-only. Thin wrapper over `groups_add()`. |
| 40 | `SYS_PASSWD` | A NULL `username` changes the caller's own password (no check needed, already authenticated); a non-NULL one resets any user's password and is root-only. |

`SYS_SU`/`SYS_USERADD`/`SYS_GROUPADD`/`SYS_PASSWD` were added later than `SYS_CHMOD`/`SYS_CHOWN`/`SYS_GETUID`/`SYS_GETGID` (v0.7 "bridge" plan) specifically to replace the equivalent kernel-native shell commands (`kernel/shell/commands/users.c`), which are now dead code — see [Shell Commands](#shell-commands).

---

## Shell Commands

The six interactive commands that drive this subsystem no longer live in the kernel. `kernel/shell/commands/users.c` still exists in the source tree but is dead code, unreachable since the shell moved to userspace (v0.7; see [`14_shell_commands.md`](14_shell_commands.md)). Today:

| Command | Where it lives | Description |
|---------|-----------------|-------------|
| `whoami` | package, `lufira-packages/base/whoami.c` | Prints the calling process's `uid`/`gid`, with resolved usernames/group names when known. |
| `chmod <mode> <path>` | package, `lufira-packages/base/chmod.c` | Sets a file's octal permission mode via `SYS_CHMOD`; owner or root only. |
| `chown <user>[:group] <path>` | package, `lufira-packages/base/chown.c` | Changes a file's owner (and optionally group) via `SYS_CHOWN`; root only. |
| `useradd <user> <password> [group]` | package, `lufira-packages/base/useradd.c` | Creates a new user via `SYS_USERADD` (auto-assigns the next free uid/gid ≥1000, creates a matching group if none given, creates a private `/home/<user>` directory). |
| `groupadd <group>` | package, `lufira-packages/base/groupadd.c` | Creates a new group via `SYS_GROUPADD` (auto-assigns the next free gid ≥1000). |
| `su [user]` | **shell builtin**, `lufira-packages/shell/shell.c` | Prompts for a password with local echo suppressed (unlike the old kernel-native version, which took the password as a plain, visible command-line argument — see [Known Limitations](#known-limitations)), then calls `SYS_SU`; root needs no password for any target, anyone else needs the target account's own password. On success, re-resolves its own identity and `cd`s to the target's home directory. |

`passwd` (`SYS_PASSWD`) is also exposed as a package, `lufira-packages/base/passwd.c`, though it predates this specific table in the original documentation pass and is not one of the original six — included here as it's part of the same identity/permission surface.

---

## Known Limitations

- **No setuid/setgid bit** — `exec()` never changes a process's identity, confirmed directly by the comment in `process_commit_exec()` (see [Process Identity](#process-identity)). There is no way for a non-root user to gain elevated privilege by running a specific program, and correspondingly no way to build a "run this one thing as root" helper the way real Unix uses setuid binaries.
- **No multi-group membership** — each user has exactly one primary `gid`; `users.h`'s own header comment describes the model as "a simple user/group database over LufiraFS text files — classic early Unix style: one primary group per user, no multi-group membership." There is no supplementary-groups list anywhere in `user_entry_t` or `process_t`.
- **Non-cryptographic password hashing** — passwords are hashed with unsalted 32-bit FNV-1a (see [Password Hashing](#password-hashing)), which is trivially brute-forceable and offers no collision resistance. Combined with `/etc/passwd` being world-readable (`perm 0644`), any local non-root process can read every account's hash.
- **`cd` does not enforce directory permissions** — `SYS_CHDIR` (`syscall.c`) never calls `lufirafs_check_access()` (see [Enforcement Coverage](#enforcement-coverage)), so a directory's own exec ("search") bit does not prevent changing into it. `ls`, by contrast, is no longer in this category: since v0.7 it's a userspace package that opens its target with a plain `SYS_OPEN(O_RDONLY)` first, which *is* permission-checked (read bit on the directory) — see [Enforcement Coverage](#enforcement-coverage).
- **`su`'s password prompt moved, but the kernel-side check did not change** — the current userspace `su` builtin (`lufira-packages/shell/shell.c`) prompts for the password separately and suppresses local echo while it's typed, unlike the old kernel-native `command_su()` (dead code), which took the password as a plain, visible command-line argument (`su <username> [password]`). The actual password check still happens once, inside the kernel (`SYS_SU` → `users_check_password()`), unchanged by this move.
- **Hardcoded default root password shipped in the seed image** — every built `disk.img` ships `root` with the same well-known password (`toor`) and an additional `guest` account with **no password at all** (see [Default and Seeded Accounts](#default-and-seeded-accounts)). This is a real security problem for any use beyond a local, single-user hobby/testing context — there is no first-boot password-change flow, and nothing warns a user to change either credential.
- **No `euid`/`egid` distinction** — a process's "real" and "effective" identity are always the same value, which is consistent with there being no setuid bit but does mean there is no mechanism at all for temporary privilege elevation or drop within a single process.

---

## Conclusion

The users/groups/permissions subsystem gives LufiraOS a recognisably Unix-shaped security model — numeric `uid`/`gid` identity, 9-bit owner/group/other permissions on every LufiraFS inode, a root account that bypasses all checks, and persistent accounts backed by plain-text `/etc/passwd`/`/etc/group` — built entirely from fixed-size in-memory tables and hand-rolled text parsing, in keeping with the rest of this freestanding kernel's minimalism. It intentionally stops short of a full Unix model: no setuid, no supplementary groups, and a non-cryptographic password scheme that is adequate for a hobby OS but must not be mistaken for a real security boundary.

For more details, refer to the source code in `kernel/system/users/`, `kernel/shell/commands/users.c`, `kernel/fs/lufirafs/lufirafs_format.h`, `kernel/fs/lufirafs/lufirafs.c`, and `kernel/system/process/`.

---

**Document Version:** 1.0
**Last Updated:** September 2026
**Project:** LufiraOS
