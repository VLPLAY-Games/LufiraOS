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

Growing the inode by 12 bytes is a breaking on-disk format change — a `disk.img` formatted by the old `mkfs_lufirafs`/kernel pair would misparse every inode under the new 76-byte stride. This is acceptable here (and is, in effect, the same reasoning the surrounding build already relies on for the format header — both the freestanding kernel driver and the hosted `tools/mkfs_lufirafs.c` include the exact same `lufirafs_format.h`, so the two can never silently disagree) because `disk.img` is a build artifact: the `Makefile`'s `$(BUILD_DIR)/disk.img` rule always runs `mkfs_lufirafs format` from scratch before populating `/etc/passwd`/`/etc/group` and everything else, for every clean build. There is no persistent installed system and therefore no migration path to maintain.

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

Two call sites wrap `lufirafs_check_access()`:

- **`check_perm()`** in `kernel/shell/commands/filesystem.c` — used by shell commands that talk to LufiraFS directly (bypassing the VFS/syscall layer). On failure it prints `"<cmd>: permission denied: <name>"` and aborts the command.
- Direct calls in `kernel/system/syscall/syscall.c` and `do_exec()` — used by syscalls and by `exec`/`run`.

| Operation | Where | Checks |
|-----------|-------|--------|
| `mkdir` | shell `check_perm(cwd_inode, ...)` | write+exec on the target directory (cwd) |
| `rm` | shell `check_perm(cwd_inode, ...)` | write+exec on the parent directory |
| `touch` | shell `check_perm(cwd_inode, ...)` | write+exec on the parent directory |
| `cat` | shell `check_perm(ino, ...)` | read on the file |
| `run` (shell) / `exec` / `SYS_EXEC` | shell `check_perm()` and `do_exec()` | exec bit on the target file |
| `write` (shell) | shell `check_perm()` | write on an existing file, or write+exec on the parent when creating |
| `cp` | shell `check_perm()` (multiple calls) | read on source; write+exec on destination parent; write on an existing destination |
| `mv` | shell `check_perm()` (multiple calls) | read+write+exec on source and its parent; write+exec on destination parent; write on an existing destination |
| `edit` | shell `check_perm()` | write+exec on the parent when creating, write on an existing file |
| `SYS_OPEN` | `syscall.c` | read/write on the target, or write+exec on the parent for `O_CREAT` |
| `SYS_MKDIR` | `syscall.c` | write+exec on the parent (via `cwd_inode`-relative resolution) |
| `SYS_RMDIR` / `SYS_UNLINK` | `syscall.c` (`sys_remove()`) | write+exec on the parent |

Two everyday operations were deliberately **not** wired up to any permission check:

| Operation | Behaviour |
|-----------|-----------|
| `ls` (`command_ls()`) | Calls `lufirafs_opendir()`/`lufirafs_readdir()` directly — no `check_perm()` call anywhere in the function. Directory listing does not enforce read permission on the directory being listed. |
| `cd` (`command_cd()`) | Calls `lufirafs_lookup()` directly to resolve the target path — no `check_perm()` call. Changing into a directory does not check its exec ("search") bit. |

This matches the pattern observed directly in the source (`grep` for `check_perm(` in `kernel/shell/commands/filesystem.c` finds calls from `mkdir`/`rm`/`touch`/`cat`/`run`/`write`/`cp`/`mv`/`edit`, but none from `command_ls()` or `command_cd()`) — a deliberate scoping decision rather than an oversight, but one worth knowing about if you're relying on directory permissions to hide contents (see [Known Limitations](#known-limitations)).

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

`tools/seed/passwd` and `tools/seed/group` are copied into `/etc/passwd`/`/etc/group` on every image build (`Makefile`'s `$(BUILD_DIR)/disk.img` target, via `mkfs_lufirafs put`). Their contents ship exactly two accounts:

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

**Boot-time default identity:** the kernel's `idle_process` is constructed directly (not via `process_create()`, since it's `kmalloc`'d by hand before the process subsystem is otherwise ready) and has `idle_process->uid = 0; idle_process->gid = 0;` set explicitly, with the source comment calling it out as the system's identity "point zero": *"idle is the identity starting point for the whole system (the first shell inherits from it via `process_create()`), so it must be root, not kmalloc garbage."* When `kernel.c` later calls `process_create("shell", shell_task)`, `current_process` at that point is still `idle_process` (uid 0), so the very first shell process inherits `uid=0`/`gid=0` — LufiraOS boots straight into a root shell, with no login prompt anywhere in the sequence. The only way to drop privilege afterward is the `su` shell command mutating `current_process->uid`/`gid` in place (see [Shell Commands](#shell-commands)).

---

## New Syscalls

Four syscalls expose identity/permission operations to userspace. Full signatures, return values, and error codes are documented in [13_syscalls.md](13_syscalls.md#filesystem--identity) — this table is a summary only.

| # | Name | Description |
|---|------|-------------|
| 19 | `SYS_CHMOD` | Sets a file's 9-bit permission mode; owner or root only. |
| 20 | `SYS_CHOWN` | Changes a file's owner and group; root only. |
| 21 | `SYS_GETUID` | Returns the calling process's `uid`. |
| 22 | `SYS_GETGID` | Returns the calling process's `gid`. |

---

## Shell Commands

Six shell commands (all in `kernel/shell/commands/users.c`) drive this subsystem interactively. Full usage and behaviour are documented in [14_shell_commands.md](14_shell_commands.md) — this table is a summary only.

| Command | Description |
|---------|-------------|
| `whoami` | Prints the calling process's `uid`/`gid`, with resolved usernames/group names when known. |
| `chmod <mode> <path>` | Sets a file's octal permission mode; owner or root only. |
| `chown <user>[:group] <path>` | Changes a file's owner (and optionally group); root only. |
| `useradd <user> <password> [group]` | Creates a new user (auto-assigns the next free uid/gid ≥1000, creates a matching group if none given, creates a private `/home/<user>` directory). |
| `groupadd <group>` | Creates a new group (auto-assigns the next free gid ≥1000). |
| `su <user> [password]` | Switches the invoking shell's own `uid`/`gid` in place; root needs no password for any target, anyone else needs the target account's own password. |

---

## Known Limitations

- **No setuid/setgid bit** — `exec()` never changes a process's identity, confirmed directly by the comment in `process_commit_exec()` (see [Process Identity](#process-identity)). There is no way for a non-root user to gain elevated privilege by running a specific program, and correspondingly no way to build a "run this one thing as root" helper the way real Unix uses setuid binaries.
- **No multi-group membership** — each user has exactly one primary `gid`; `users.h`'s own header comment describes the model as "a simple user/group database over LufiraFS text files — classic early Unix style: one primary group per user, no multi-group membership." There is no supplementary-groups list anywhere in `user_entry_t` or `process_t`.
- **Non-cryptographic password hashing** — passwords are hashed with unsalted 32-bit FNV-1a (see [Password Hashing](#password-hashing)), which is trivially brute-forceable and offers no collision resistance. Combined with `/etc/passwd` being world-readable (`perm 0644`), any local non-root process can read every account's hash.
- **`su` takes the password as a plain, visible shell argument** — `command_su()` reads the password straight out of the typed command line (`su <username> [password]`); there is no masked/hidden-input prompt. The source comment acknowledges this directly: `// Видимый ввод пароля (не маскируется) — сознательное упрощение` ("visible password input, not masked — a deliberate simplification"). Anything that can see the shell's input (or command history, if any were kept) sees the password in cleartext.
- **`ls` and `cd` do not enforce directory permissions** — neither `command_ls()` nor `command_cd()` calls the shared `check_perm()` helper (see [Enforcement Coverage](#enforcement-coverage)), so a directory's own read/exec bits do not prevent listing its contents or changing into it. Only operations on the files *inside* a directory (or on the directory as a write target) are checked.
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
