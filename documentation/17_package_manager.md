# Package Manager

This document describes LufiraOS's package format (`.lpg`) and its installer, `dlpg`. The format itself (`tools/lpg_format.h`) lives in this repository, shared by the host-side packer and the userspace installer; the packages themselves, `dlpg`'s source, and the published index live in the sibling repository [`lufira-packages`](https://github.com/VLPLAY-Games/lufira-packages). This document covers the format and `dlpg`'s behavior; see that repository for the actual package list.

---

## Table of Contents

1. [Overview](#overview)
2. [The `.lpg` Format](#the-lpg-format)
3. [Installed-Package Bookkeeping](#installed-package-bookkeeping)
4. [`dlpg` Subcommands](#dlpg-subcommands)
   - [install / update](#install--update)
   - [list](#list)
   - [remove](#remove)
   - [sync](#sync)
   - [upgrade](#upgrade)
5. [The Published Index (`lufira-packages`)](#the-published-index-lufira-packages)
6. [Dependencies](#dependencies)
7. [Known Limitations](#known-limitations)
8. [Conclusion](#conclusion)

---

## Overview

Before v0.7, every driver and command shipped built into the kernel binary. `.lpg` + `dlpg` (`lufira-packages/base/dlpg.c`) replace that for everything that can run in ring 3: coreutils, GUI apps, and the package manager itself are each a standalone `.lpg`, installed independently under `/bin`. `dlpg` needs no new syscalls beyond what every other userspace program already has — `open`/`read`/`write`/`close`/`seek`/`mkdir`/`chmod`/`unlink` — plus, as of this release, `SYS_NET_FETCH` for `sync`/`upgrade` (see [`16_networking.md`](16_networking.md#https-client-and-sys_net_fetch)).

There is no archive library anywhere in this project (kernel or userspace) — `.lpg` is a small hand-rolled binary format, the same philosophy as LufiraFS's own on-disk format.

---

## The `.lpg` Format

Defined in `tools/lpg_format.h`, shared byte-for-byte between the host-side packer (`lpg_pack.c`, plain `gcc`) and `dlpg` (freestanding, built against this kernel's libc) — the header only includes `<stdint.h>`, available in both worlds.

**Layout:**

```
[lpg_header_t]
[lpg_dependency_t] * header.dep_count
[lpg_file_entry_t] * header.file_count
[file data, concatenated — each entry's `offset` is absolute from the start of the .lpg]
```

```c
#define LPG_MAGIC "LPG1"
#define LPG_NAME_MAX 32
#define LPG_PATH_MAX 64
#define LPG_MAX_DEPS  16
#define LPG_MAX_FILES 64

typedef struct __attribute__((packed)) {
    uint16_t major, minor, patch;
} lpg_version_t;

typedef struct __attribute__((packed)) {
    char magic[4];           // "LPG1"
    char name[32];
    lpg_version_t version;
    uint8_t category;        // 0 = base, 1 = user
    uint8_t reserved[1];
    uint32_t dep_count;
    uint32_t file_count;
} lpg_header_t;

typedef struct __attribute__((packed)) {
    char name[32];
    lpg_version_t min_version;
} lpg_dependency_t;

typedef struct __attribute__((packed)) {
    char path[64];            // install destination, e.g. "/bin/du.elf"
    uint32_t size;
    uint32_t mode;             // e.g. 0755
    uint32_t offset;           // absolute offset of this file's data
} lpg_file_entry_t;
```

`LPG_MAX_DEPS`/`LPG_MAX_FILES` are sanity ceilings checked before any allocation sized by `dep_count`/`file_count` — a corrupted or hostile `.lpg` claiming an absurd count is rejected up front, not used to size a `malloc()`.

`lpg_version_gte(a, b)` / `lpg_version_gt(a, b)` compare `(major, minor, patch)` lexicographically — `gte` for dependency checks ("installed version is at least the required minimum"), the strict `gt` for `upgrade` (so a remote version merely *equal* to the local one is correctly treated as "nothing to do," not "reinstall").

---

## Installed-Package Bookkeeping

All under `/etc/packages/` (created on first `dlpg` invocation):

| Path | Contents |
|------|----------|
| `/etc/packages/installed` | One line per installed package: `name:major.minor.patch:category`. |
| `/etc/packages/<name>.files` | One absolute path per line — every file that package installed, used by `remove` to know what to delete. |
| `/etc/packages/remote_index.json` | The last `dlpg sync`'d copy of the published index (see below) — `upgrade` reads this, it does not re-fetch it. |

No database beyond these flat files — `dlpg` parses/rewrites `installed` in full on every operation that changes it (same pattern as `/etc/passwd`/`/etc/group`, see [`15_users_permissions.md`](15_users_permissions.md)).

---

## `dlpg` Subcommands

### install / update

```
dlpg install <path.lpg>   # fails if the package is already installed
dlpg update  <path.lpg>   # (re)installs even if already installed
```

Both: validate the magic/header, check every dependency in the package against `installed` (version-checked with `lpg_version_gte()`), write out every file (creating/`chmod`ing each per its entry), write the receipt (`<name>.files`), and update `installed`. A dependency failure or a write failure aborts **before** anything else is touched — no partial installs. `update` is the only difference between the two: whether an already-present package is an error or an expected reinstall.

### list

Prints every installed package's name, version, and category from `/etc/packages/installed`.

### remove

Reads `<name>.files`, `unlink()`s every path in it, deletes the receipt, and drops the entry from `installed`.

### sync

```
dlpg sync
```

Fetches the published `index.json` (see below) via `SYS_NET_FETCH`, saves it to `/etc/packages/remote_index.json`, and reports how many packages it lists. This is the *only* network operation in `sync` — it does not install or download anything else. Prints a short "connecting... this can take a while" notice before the fetch and a result line after, since the single blocking `SYS_NET_FETCH` call gives no way to report progress *during* the fetch itself — DNS, the TCP/TLS handshake, and the HTTP exchange all happen inside that one call.

### upgrade

```
dlpg upgrade          # every installed package
dlpg upgrade <name>    # just one
```

Reads the index cached by the last `sync` (fails with a clear message if none exists yet — it never fetches a fresh one itself). For each installed package also present in the index: parses its `"name"`/`"version"`/`"lpg"` fields out of the index (a small hand-written scanner, not a general JSON parser — see below), and if the index's version is strictly newer (`lpg_version_gt()`), downloads that package's `.lpg` via its own `SYS_NET_FETCH` call to a staging path, then installs it exactly like `dlpg update` would. Never installs a package that isn't already present — that's `install`'s job. Reports a `checked/upgraded/failed` summary at the end.

**The index parser is deliberately not a general JSON parser.** It knows only the one flat shape `build_index.py` (in `lufira-packages`) always produces: a top-level `"packages"` array of objects with simple string fields, no nesting. That's a reasonable simplification only *because* this repository controls that format end-to-end — it is not a general-purpose JSON reader and would not cope with arbitrary JSON.

---

## The Published Index (`lufira-packages`)

`lufira-packages/index.json`, regenerated by that repository's `build_index.py` on every build, lists every package with its name, version, category, dependencies, size, sha256, and a real download URL (`"lpg"`) pointing at that same repository's own `release/` directory, served via `raw.githubusercontent.com` — no separate release server. Two more top-level entries, `"shell_elf"` and `"libc_so"`, publish the shell binary and the shared libc the same way (not `.lpg` packages — direct-staged runtime files, see [`12_elf_processes.md`](12_elf_processes.md#dynamic-linking-libcso)); `LufiraOS-Builder` fetches those too when assembling a disk image without a local `lufira-packages` source checkout.

`REMOTE_INDEX_URL` in `dlpg.c` and the equivalent constant in `LufiraOS-Builder`'s `config.py` must point at the same repository/branch — there is no single shared source of truth for that URL across the two codebases, just a comment in each cross-referencing the other.

---

## Dependencies

| Component | Depends On | Purpose |
|-----------|------------|---------|
| `dlpg` | `SYS_NET_FETCH` (`SYS_NET_FETCH`, [`16_networking.md`](16_networking.md)) | `sync`/`upgrade` only — `install`/`update`/`list`/`remove` are entirely local/offline. |
| `dlpg` | VFS syscalls (`open`/`read`/`write`/`mkdir`/`chmod`/`unlink`) | Everything else — no package-manager-specific syscalls exist. |
| `tools/lpg_format.h` | Nothing kernel-specific | Plain `<stdint.h>` struct definitions, shared with the host-side `lpg_pack.c`. |
| `LufiraOS-Builder` | `lufira-packages`' `index.json` | Default image-build path downloads prebuilt `.lpg`s from it instead of building from source (see that repository's own README). |

---

## Known Limitations

- **No signature verification on `.lpg` files or the index** beyond whatever TLS's own (chain-unvalidated — see [`16_networking.md`](16_networking.md#tls-12)) transport integrity provides. `sync`/`upgrade` check a package's sha256 against what the index claims, but nothing independently proves the index itself came from a trusted publisher.
- **`sync`/`upgrade` give no mid-fetch progress** — a single blocking `SYS_NET_FETCH` call covers DNS through the parsed HTTP response; `dlpg` can only report before and after it, not during.
- **The index parser is special-purpose**, not a general JSON reader — see [upgrade](#upgrade).
- **No automatic dependency installation** — a missing dependency aborts `install`/`update` with an error; the admin installs it by hand.

---

## Conclusion

The package manager is a small, from-scratch format and installer sized to this project's actual needs — no archive library, no general JSON parser, no signature scheme beyond what TLS happens to provide. `sync`/`upgrade` turn it from "copy a `.lpg` onto the disk image at build time" into something that can reach out over a real network and pull a newer package from a real GitHub-hosted repository, which is new this release and the reason the networking stack documented in [`16_networking.md`](16_networking.md) needed to grow a DNS resolver and a TLS client in the first place.

For more details, refer to `tools/lpg_format.h` in this repository and `base/dlpg.c`/`build_index.py` in `lufira-packages`.

---

**Document Version:** 1.0
**Last Updated:** October 2026
**Project:** LufiraOS
